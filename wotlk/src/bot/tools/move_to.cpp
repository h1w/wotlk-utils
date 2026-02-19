#include "move_to.h"
#include "../../game/game.h"
#include "../../game/movement.h"
#include "../../game/world.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdio>

namespace bot {

MoveToTool::MoveToTool(const game::Vec3& target, float arrivalDist)
    : m_target(target)
    , m_arrivalDist(arrivalDist)
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

    m_lastPos = player->GetPosition();
    m_lastProgressTick = GetTickCount64();
    m_retries = 0;
    m_status = ToolStatus::Running;

    IssueCTM();
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

    // Arrived?
    if (dist <= m_arrivalDist) {
        m_status = ToolStatus::Completed;
        return;
    }

    // Check progress for stuck detection
    float moved = pos.DistanceTo(m_lastPos);
    uint64_t now = GetTickCount64();

    if (moved >= kStuckThreshold) {
        // Making progress — reset
        m_lastPos = pos;
        m_lastProgressTick = now;
    } else if (now - m_lastProgressTick > kStuckTimeoutMs) {
        // Stuck — retry
        m_retries++;
        if (m_retries > kMaxRetries) {
            m_status = ToolStatus::Failed;
            return;
        }
        m_lastPos = pos;
        m_lastProgressTick = now;
        IssueCTM();
    }
}

void MoveToTool::Abort()
{
    if (m_status == ToolStatus::Running || m_status == ToolStatus::Pending) {
        game::movement::StopCTM();
        game::movement::StopMoving();
        m_status = ToolStatus::Cancelled;
    }
}

std::string MoveToTool::Describe() const
{
    char buf[96];
    snprintf(buf, sizeof(buf), "MoveTo (%.1f, %.1f, %.1f)", m_target.x, m_target.y, m_target.z);
    return buf;
}

float MoveToTool::GetDistanceRemaining() const
{
    auto player = game::GetLocalPlayer();
    if (!player) return 0.0f;
    return player->GetPosition().DistanceTo(m_target);
}

void MoveToTool::IssueCTM()
{
    game::movement::ClickToMove(m_target);
}

} // namespace bot
