#include "follow_route.h"
#include "../../game/game.h"
#include "../../game/movement.h"
#include "../../game/world.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdio>

namespace bot {

FollowRouteTool::FollowRouteTool(std::vector<game::Vec3> waypoints, bool loop)
    : m_waypoints(std::move(waypoints))
    , m_loop(loop)
{
}

void FollowRouteTool::Start()
{
    if (!game::world::IsInGame() || m_waypoints.empty()) {
        m_status = ToolStatus::Failed;
        return;
    }

    m_currentIndex = 0;
    m_startTick = GetTickCount64();
    m_lastStuckCheckTick = m_startTick;
    m_stuckCount = 0;
    m_status = ToolStatus::Running;

    auto player = game::GetLocalPlayer();
    if (player)
        m_lastPosition = player->GetPosition();

    IssueCTMToCurrentWP();
}

void FollowRouteTool::Tick()
{
    if (m_status != ToolStatus::Running)
        return;

    uint64_t now = GetTickCount64();

    // Timeout
    if (now - m_startTick > kTimeoutMs) {
        game::movement::StopCTM();
        m_status = ToolStatus::Failed;
        return;
    }

    auto player = game::GetLocalPlayer();
    if (!player) {
        m_status = ToolStatus::Failed;
        return;
    }

    game::Vec3 myPos = player->GetPosition();
    const game::Vec3& targetWP = m_waypoints[m_currentIndex];
    float dist = myPos.Distance2D(targetWP);

    // Arrived at current waypoint?
    if (dist < kArrivalThreshold) {
        AdvanceToNext();
        return;
    }

    // Stuck detection (every 3s)
    if (now - m_lastStuckCheckTick > kStuckCheckMs) {
        float moved = myPos.Distance2D(m_lastPosition);
        m_lastPosition = myPos;
        m_lastStuckCheckTick = now;

        if (moved < kStuckThreshold) {
            m_stuckCount++;
            if (m_stuckCount > kMaxStuckRetries) {
                game::movement::StopCTM();
                m_status = ToolStatus::Failed;
                return;
            }
            // Try to unstick: jump + re-issue CTM
            game::movement::Jump();
            IssueCTMToCurrentWP();
        } else {
            m_stuckCount = 0;
        }
    }
}

void FollowRouteTool::Abort()
{
    if (m_status == ToolStatus::Running || m_status == ToolStatus::Pending) {
        game::movement::StopCTM();
        m_status = ToolStatus::Cancelled;
    }
}

void FollowRouteTool::AdvanceToNext()
{
    m_currentIndex++;

    if (m_currentIndex >= m_waypoints.size()) {
        if (m_loop) {
            m_currentIndex = 0;
        } else {
            // Don't call StopCTM — the WoW CTM for the last waypoint is still
            // active and will naturally stop the character when it arrives.
            // Calling StopCTM(action=0x0D) cancels the CTM but doesn't clear
            // the movement flags, causing the character to run indefinitely.
            m_status = ToolStatus::Completed;
            return;
        }
    }

    m_stuckCount = 0;
    IssueCTMToCurrentWP();
}

void FollowRouteTool::IssueCTMToCurrentWP()
{
    if (m_currentIndex < m_waypoints.size())
        game::movement::ClickToMove(m_waypoints[m_currentIndex]);
}

float FollowRouteTool::GetDistanceToCurrentWP() const
{
    auto player = game::GetLocalPlayer();
    if (!player || m_currentIndex >= m_waypoints.size())
        return 0.0f;
    return player->GetPosition().Distance2D(m_waypoints[m_currentIndex]);
}

float FollowRouteTool::GetProgress() const
{
    if (m_waypoints.empty()) return 1.0f;
    return static_cast<float>(m_currentIndex) / static_cast<float>(m_waypoints.size());
}

std::string FollowRouteTool::Describe() const
{
    char buf[128];
    snprintf(buf, sizeof(buf), "Follow Route [%zu/%zu]%s",
             m_currentIndex + 1, m_waypoints.size(),
             m_loop ? " (loop)" : "");
    return buf;
}

} // namespace bot
