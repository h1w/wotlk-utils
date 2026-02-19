#include "nav_helper.h"
#include "../game/game.h"
#include "../game/movement.h"
#include "../game/world.h"
#include "../navigation/pathfinder.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <glog/logging.h>

namespace bot {

bool NavHelper::StartNavTo(const game::Vec3& target)
{
    m_waypoints.clear();
    m_currentIndex = 0;
    m_stuckCount = 0;
    m_status = Status::Idle;

    if (!game::world::IsInGame())
        return false;

    auto player = game::GetLocalPlayer();
    if (!player)
        return false;

    game::Vec3 start = player->GetPosition();

    auto result = nav::Pathfinder::Instance().FindPath(start, target);
    if (!result.success || result.waypoints.empty())
        return false;

    m_waypoints = std::move(result.waypoints);
    m_currentIndex = 0;
    m_lastPosition = start;
    m_lastStuckCheckTick = GetTickCount64();
    m_status = Status::Moving;

    // Skip first waypoint if it's very close (it's our current position)
    if (m_waypoints.size() > 1 && start.Distance2D(m_waypoints[0]) < kArrivalThreshold)
        m_currentIndex = 1;

    IssueCTMToCurrentWP();

    LOG(INFO) << "[NavHelper] Started path with " << m_waypoints.size()
              << " waypoints (starting at index " << m_currentIndex << ")";
    return true;
}

NavHelper::Status NavHelper::Tick()
{
    if (m_status != Status::Moving)
        return m_status;

    auto player = game::GetLocalPlayer();
    if (!player) {
        m_status = Status::Failed;
        return m_status;
    }

    game::Vec3 myPos = player->GetPosition();
    const game::Vec3& targetWP = m_waypoints[m_currentIndex];
    float dist = myPos.Distance2D(targetWP);

    // Arrived at current waypoint?
    if (dist < kArrivalThreshold) {
        AdvanceToNext();
        return m_status;
    }

    // Stuck detection (every 3s)
    uint64_t now = GetTickCount64();
    if (now - m_lastStuckCheckTick > kStuckCheckMs) {
        float moved = myPos.Distance2D(m_lastPosition);
        m_lastPosition = myPos;
        m_lastStuckCheckTick = now;

        if (moved < kStuckThreshold) {
            m_stuckCount++;
            if (m_stuckCount > kMaxStuckRetries) {
                LOG(WARNING) << "[NavHelper] Stuck after " << kMaxStuckRetries << " retries, failing";
                game::movement::StopCTM();
                m_status = Status::Failed;
                return m_status;
            }
            // Try to unstick: jump + re-issue CTM
            game::movement::Jump();
            IssueCTMToCurrentWP();
        } else {
            m_stuckCount = 0;
        }
    }

    return m_status;
}

void NavHelper::Stop()
{
    if (m_status == Status::Moving) {
        game::movement::StopCTM();
        m_status = Status::Idle;
    }
}

void NavHelper::AdvanceToNext()
{
    m_currentIndex++;

    if (m_currentIndex >= m_waypoints.size()) {
        // Reached final waypoint — don't call StopCTM, let the calling tool
        // issue its own CTM (which replaces ours) or let WoW finish naturally.
        m_status = Status::Arrived;
        return;
    }

    m_stuckCount = 0;
    IssueCTMToCurrentWP();
}

void NavHelper::IssueCTMToCurrentWP()
{
    if (m_currentIndex < m_waypoints.size())
        game::movement::ClickToMove(m_waypoints[m_currentIndex]);
}

} // namespace bot
