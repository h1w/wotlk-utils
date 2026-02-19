#include "move_to.h"
#include "../../game/game.h"
#include "../../game/movement.h"
#include "../../game/world.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdio>

#include <glog/logging.h>

namespace bot {

MoveToTool::MoveToTool(const game::Vec3& target)
    : m_target(target)
{
}

void MoveToTool::Start()
{
    if (!game::world::IsInGame()) {
        m_status = ToolStatus::Failed;
        return;
    }

    auto player = game::GetLocalPlayer();
    if (!player) {
        m_status = ToolStatus::Failed;
        return;
    }

    m_status = ToolStatus::Running;

    // Try navmesh pathfinding first
    if (m_nav.StartNavTo(m_target)) {
        m_useNav = true;
        LOG(INFO) << "[MoveToTool] Using navmesh path";
    } else {
        // Fallback to direct CTM
        m_useNav = false;
        LOG(INFO) << "[MoveToTool] Navmesh unavailable, using direct CTM";
        StartDirectCTM();
    }
}

void MoveToTool::Tick()
{
    if (m_status != ToolStatus::Running)
        return;

    auto player = game::GetLocalPlayer();
    if (!player) {
        m_status = ToolStatus::Failed;
        return;
    }

    game::Vec3 pos = player->GetPosition();
    float dist = pos.DistanceTo(m_target);

    // Arrived? (check regardless of nav mode)
    if (dist <= kArrivalDist) {
        m_status = ToolStatus::Completed;
        return;
    }

    if (m_useNav) {
        auto navStatus = m_nav.Tick();
        switch (navStatus) {
        case NavHelper::Status::Arrived:
            // Nav thinks we arrived at last waypoint — check actual distance
            if (dist <= kArrivalDist) {
                m_status = ToolStatus::Completed;
            } else {
                // Close but not quite — finish with direct CTM
                m_useNav = false;
                StartDirectCTM();
            }
            break;
        case NavHelper::Status::Failed:
            // Nav failed (stuck) — fall back to direct CTM
            LOG(WARNING) << "[MoveToTool] Nav failed, falling back to direct CTM";
            m_useNav = false;
            StartDirectCTM();
            break;
        case NavHelper::Status::Moving:
            // Still navigating
            break;
        default:
            break;
        }
    } else {
        // Direct CTM mode — stuck detection
        float moved = pos.DistanceTo(m_lastPos);
        uint64_t now = GetTickCount64();

        if (moved >= kStuckThreshold) {
            m_lastPos = pos;
            m_lastProgressTick = now;
        } else if (now - m_lastProgressTick > kStuckTimeoutMs) {
            m_retries++;
            if (m_retries > kMaxRetries) {
                m_status = ToolStatus::Failed;
                return;
            }
            m_lastPos = pos;
            m_lastProgressTick = now;
            game::movement::Jump();
            game::movement::ClickToMove(m_target);
        }
    }
}

void MoveToTool::Abort()
{
    if (m_status == ToolStatus::Running || m_status == ToolStatus::Pending) {
        if (m_useNav)
            m_nav.Stop();
        else
            game::movement::StopCTM();
        game::movement::StopMoving();
        m_status = ToolStatus::Cancelled;
    }
}

std::string MoveToTool::Describe() const
{
    char buf[96];
    snprintf(buf, sizeof(buf), "MoveTo (%.1f, %.1f, %.1f)%s",
             m_target.x, m_target.y, m_target.z,
             m_useNav ? " [nav]" : " [direct]");
    return buf;
}

float MoveToTool::GetDistanceRemaining() const
{
    auto player = game::GetLocalPlayer();
    if (!player) return 0.0f;
    return player->GetPosition().DistanceTo(m_target);
}

void MoveToTool::StartDirectCTM()
{
    auto player = game::GetLocalPlayer();
    if (player) {
        m_lastPos = player->GetPosition();
        m_lastProgressTick = GetTickCount64();
    }
    m_retries = 0;
    game::movement::ClickToMove(m_target);
}

} // namespace bot
