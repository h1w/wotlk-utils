#include "attack.h"
#include "../../game/game.h"
#include "../../game/movement.h"
#include "../../game/world.h"
#include "../../game/object_manager.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdio>

namespace bot {

AttackTool::AttackTool(game::GUID targetGuid)
    : m_targetGuid(targetGuid)
{
}

void AttackTool::Start()
{
    if (!game::world::IsInGame()) {
        m_status = ToolStatus::Failed;
        return;
    }

    // Validate target exists
    uintptr_t ptr = game::objmgr::GetObjectPtr(m_targetGuid);
    if (ptr == 0) {
        m_status = ToolStatus::Failed;
        return;
    }

    game::Unit target(ptr);
    m_targetName = target.GetUnitName();

    if (target.IsDead()) {
        m_status = ToolStatus::Completed;
        return;
    }

    m_status = ToolStatus::Running;

    // Select and attack
    game::SelectTarget(m_targetGuid);
    IssueAttack();
}

void AttackTool::Tick()
{
    if (m_status != ToolStatus::Running)
        return;

    // Check target still exists
    uintptr_t ptr = game::objmgr::GetObjectPtr(m_targetGuid);
    if (ptr == 0) {
        m_status = ToolStatus::Failed;
        return;
    }

    game::Unit target(ptr);

    // Target dead?
    if (target.IsDead()) {
        m_status = ToolStatus::Completed;
        return;
    }

    // Periodically re-issue attack (in case CTM stopped)
    uint64_t now = GetTickCount64();
    if (now - m_lastAttackTick > kReAttackIntervalMs)
        IssueAttack();
}

void AttackTool::Abort()
{
    if (m_status == ToolStatus::Running || m_status == ToolStatus::Pending) {
        game::movement::StopCTM();
        m_status = ToolStatus::Cancelled;
    }
}

std::string AttackTool::Describe() const
{
    char buf[128];
    if (m_targetName.empty())
        snprintf(buf, sizeof(buf), "Attack (GUID: 0x%llX)", m_targetGuid);
    else
        snprintf(buf, sizeof(buf), "Attack \"%s\"", m_targetName.c_str());
    return buf;
}

void AttackTool::IssueAttack()
{
    uintptr_t ptr = game::objmgr::GetObjectPtr(m_targetGuid);
    if (ptr == 0) return;

    game::Unit target(ptr);
    game::Vec3 targetPos = target.GetPosition();
    game::movement::ClickToMoveAttack(m_targetGuid, targetPos);
    m_lastAttackTick = GetTickCount64();
}

} // namespace bot
