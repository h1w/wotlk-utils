#include "attack.h"
#include "../../game/game.h"
#include "../../game/movement.h"
#include "../../game/world.h"
#include "../../game/object_manager.h"
#include "../../game/lua_bridge.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdio>

#include <glog/logging.h>

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

    uintptr_t ptr = game::objmgr::GetObjectPtr(m_targetGuid);
    if (ptr == 0) {
        m_status = ToolStatus::Failed;
        return;
    }

    game::Unit target(ptr);
    m_targetName = target.GetUnitName();
    m_lastKnownPos = target.GetPosition();

    if (target.IsDead()) {
        m_status = ToolStatus::Completed;
        return;
    }

    m_status = ToolStatus::Running;
    m_targetLostTick = 0;
    m_useNav = false;

    auto player = game::GetLocalPlayer();
    game::Vec3 myPos = player ? player->GetPosition() : game::Vec3{};
    float dist = player ? myPos.DistanceTo(m_lastKnownPos) : 0.0f;

    m_lastPos = myPos;
    m_lastProgressTick = GetTickCount64();

    game::SelectTarget(m_targetGuid);

    // If target is far, use navmesh to approach
    if (dist > kNavSwitchRange && m_nav.StartNavTo(m_lastKnownPos)) {
        m_useNav = true;
        LOG(INFO) << "[AttackTool] Using navmesh to approach target (" << dist << " yards)";
    } else {
        IssueCTMAttack();
        EnsureAutoAttack();
    }
}

void AttackTool::Tick()
{
    if (m_status != ToolStatus::Running)
        return;

    uint64_t now = GetTickCount64();

    // ---- Target validation ----

    uintptr_t ptr = game::objmgr::GetObjectPtr(m_targetGuid);
    if (ptr == 0) {
        if (m_useNav) {
            m_nav.Stop();
            m_useNav = false;
        }

        // Grace period
        if (m_targetLostTick == 0)
            m_targetLostTick = now;

        if (now - m_targetLostTick > kTargetLostGraceMs) {
            m_status = ToolStatus::Failed;
            return;
        }

        // During grace, try to re-engage periodically
        if (now - m_lastCTMTick > kProgressCheckMs) {
            game::SelectTarget(m_targetGuid);
            game::movement::ClickToMoveAttack(m_targetGuid, m_lastKnownPos);
            EnsureAutoAttack();
            m_lastCTMTick = now;
        }
        return;
    }

    m_targetLostTick = 0;

    game::Unit target(ptr);
    m_lastKnownPos = target.GetPosition();

    if (target.IsDead()) {
        if (m_useNav) m_nav.Stop();
        m_status = ToolStatus::Completed;
        return;
    }

    // ---- Always ensure target is selected ----
    // This is cheap (just a Lua check) and critical for resilience.
    bool hasTarget = game::lua::GetBool("UnitExists('target')");
    if (!hasTarget)
        game::SelectTarget(m_targetGuid);

    auto player = game::GetLocalPlayer();
    if (!player) {
        m_status = ToolStatus::Failed;
        return;
    }

    game::Vec3 myPos = player->GetPosition();
    float dist = myPos.DistanceTo(m_lastKnownPos);

    // ---- Nav approach mode ----

    if (m_useNav) {
        if (dist <= kNavSwitchRange) {
            m_nav.Stop();
            m_useNav = false;
            LOG(INFO) << "[AttackTool] Within " << kNavSwitchRange << "y, switching to direct attack";
            IssueCTMAttack();
            EnsureAutoAttack();
            m_lastPos = myPos;
            m_lastProgressTick = now;
            return;
        }

        auto navStatus = m_nav.Tick();
        if (navStatus == NavHelper::Status::Arrived || navStatus == NavHelper::Status::Failed) {
            m_useNav = false;
            if (navStatus == NavHelper::Status::Failed)
                LOG(WARNING) << "[AttackTool] Nav failed, switching to direct attack";
            IssueCTMAttack();
            EnsureAutoAttack();
            m_lastPos = myPos;
            m_lastProgressTick = now;
        }
        return;
    }

    // ---- Direct attack mode ----

    if (dist <= kMeleeRange) {
        // IN MELEE RANGE — just keep auto-attack running via Lua.
        // No CTM needed (CTM gets cancelled by RMB camera rotation anyway).
        if (now - m_lastAutoAttackTick > kAutoAttackRecheckMs)
            EnsureAutoAttack();

        // Reset progress tracking (we're in range, all good)
        m_lastPos = myPos;
        m_lastProgressTick = now;
    } else {
        // OUT OF MELEE RANGE — should be walking to target via CTM.
        // Two checks:
        //   1) Progress check every 1s: detect if CTM was cancelled (RMB rotation)
        //      → if stalled, re-issue immediately
        //   2) Periodic refresh every 3s: re-issue CTM to track moving target

        // Progress check
        if (now - m_lastProgressTick >= kProgressCheckMs) {
            float moved = myPos.DistanceTo(m_lastPos);
            m_lastPos = myPos;
            m_lastProgressTick = now;

            if (moved < kStuckThreshold) {
                // Character is NOT moving but should be → CTM was cancelled (RMB, etc.)
                // Re-issue immediately
                IssueCTMAttack();
                EnsureAutoAttack();
                return;
            }
        }

        // Periodic CTM refresh (target may have moved)
        if (now - m_lastCTMTick > kCTMRefreshMs) {
            IssueCTMAttack();
        }
    }
}

void AttackTool::Abort()
{
    if (m_status == ToolStatus::Running || m_status == ToolStatus::Pending) {
        if (m_useNav)
            m_nav.Stop();
        else
            game::movement::StopCTM();
        m_status = ToolStatus::Cancelled;
    }
}

std::string AttackTool::Describe() const
{
    char buf[128];
    if (m_targetName.empty())
        snprintf(buf, sizeof(buf), "Attack (GUID: 0x%llX)%s",
                 m_targetGuid, m_useNav ? " [nav]" : "");
    else
        snprintf(buf, sizeof(buf), "Attack \"%s\"%s",
                 m_targetName.c_str(), m_useNav ? " [nav]" : "");
    return buf;
}

void AttackTool::IssueCTMAttack()
{
    game::SelectTarget(m_targetGuid);
    game::movement::ClickToMoveAttack(m_targetGuid, m_lastKnownPos);
    m_lastCTMTick = GetTickCount64();
}

void AttackTool::EnsureAutoAttack()
{
    game::lua::Execute("AttackTarget()");
    m_lastAutoAttackTick = GetTickCount64();
}

} // namespace bot
