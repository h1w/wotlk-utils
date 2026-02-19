#include "loot.h"
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

static constexpr float kLootRange = 6.0f;

LootTool::LootTool(game::GUID targetGuid)
    : m_targetGuid(targetGuid)
{
}

void LootTool::Start()
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
    m_targetPos = target.GetPosition();

    // Select the target first
    game::SelectTarget(m_targetGuid);

    m_phase = Phase::Approaching;
    m_phaseStartTick = GetTickCount64();
    m_status = ToolStatus::Running;
    m_useNav = false;

    // If far from corpse, use navmesh to approach
    auto player = game::GetLocalPlayer();
    float dist = player ? player->GetPosition().DistanceTo(m_targetPos) : 0.0f;

    if (dist > kNavSwitchRange && m_nav.StartNavTo(m_targetPos)) {
        m_useNav = true;
        LOG(INFO) << "[LootTool] Using navmesh to approach corpse (" << dist << " yards)";
    } else {
        // Close enough or no navmesh — direct CTM interact
        game::movement::ClickToMoveInteract(m_targetGuid, m_targetPos);
    }
}

void LootTool::Tick()
{
    if (m_status != ToolStatus::Running)
        return;

    uint64_t now = GetTickCount64();

    switch (m_phase) {
    case Phase::Approaching: {
        // Timeout?
        if (now - m_phaseStartTick > kApproachTimeoutMs) {
            if (m_useNav)
                m_nav.Stop();
            m_status = ToolStatus::Failed;
            return;
        }

        // Check if loot window is already open
        int numItems = game::lua::GetInt("GetNumLootItems()");
        if (numItems > 0) {
            if (m_useNav)
                m_nav.Stop();
            m_lootWindowSeen = true;
            m_phase = Phase::Looting;
            m_phaseStartTick = now;

            // Auto-loot all items
            for (int i = 1; i <= numItems; ++i)
                game::lua::Executef("LootSlot(%d)", i);
            return;
        }

        // Nav approach mode
        if (m_useNav) {
            // Check distance to corpse — switch to direct interact if close enough
            auto player = game::GetLocalPlayer();
            float dist = player ? player->GetPosition().DistanceTo(m_targetPos) : 999.0f;

            if (dist <= kNavSwitchRange) {
                // Close enough — stop nav, switch to direct CTM interact
                m_nav.Stop();
                m_useNav = false;
                LOG(INFO) << "[LootTool] Within " << kNavSwitchRange << "y, switching to direct interact";
                game::SelectTarget(m_targetGuid);
                game::movement::ClickToMoveInteract(m_targetGuid, m_targetPos);
                m_lastReissueTick = now;
                return;
            }

            auto navStatus = m_nav.Tick();
            if (navStatus == NavHelper::Status::Arrived || navStatus == NavHelper::Status::Failed) {
                // Nav done or failed — switch to direct interact
                m_useNav = false;
                if (navStatus == NavHelper::Status::Failed)
                    LOG(WARNING) << "[LootTool] Nav failed, switching to direct interact";
                game::SelectTarget(m_targetGuid);
                game::movement::ClickToMoveInteract(m_targetGuid, m_targetPos);
                m_lastReissueTick = now;
            }
            return;
        }

        // Direct CTM mode (existing logic)

        // If in range but no loot window yet, try InteractUnit via Lua as fallback
        auto player = game::GetLocalPlayer();
        if (player) {
            float dist = player->GetPosition().DistanceTo(m_targetPos);
            if (dist <= kLootRange) {
                if (m_firstInRangeTick == 0)
                    m_firstInRangeTick = now;

                // We've been in range long enough with no loot window — mob has no loot
                if (now - m_firstInRangeTick > kNoLootTimeoutMs) {
                    m_status = ToolStatus::Completed;
                    return;
                }

                if (now - m_lastInteractTick > 1000) {
                    game::movement::StopCTM();
                    game::SelectTarget(m_targetGuid);
                    game::lua::Execute("InteractUnit(\"target\")");
                    m_lastInteractTick = now;
                }
            }
        }

        // Re-issue CTM Interact periodically (in case it got interrupted)
        if (now - m_lastReissueTick > 3000) {
            uintptr_t ptr = game::objmgr::GetObjectPtr(m_targetGuid);
            if (ptr != 0) {
                game::Unit target(ptr);
                m_targetPos = target.GetPosition();
            }
            game::SelectTarget(m_targetGuid);
            game::movement::ClickToMoveInteract(m_targetGuid, m_targetPos);
            m_lastReissueTick = now;
        }
        break;
    }

    case Phase::Looting: {
        // Timeout?
        if (now - m_phaseStartTick > kLootTimeoutMs) {
            game::lua::Execute("CloseLoot()");
            m_status = ToolStatus::Completed;
            return;
        }

        // Check if loot window closed (all items picked)
        int numItems = game::lua::GetInt("GetNumLootItems()");
        if (numItems == 0) {
            m_status = ToolStatus::Completed;
            return;
        }

        // Try to loot remaining items
        for (int i = 1; i <= numItems; ++i)
            game::lua::Executef("LootSlot(%d)", i);
        break;
    }

    case Phase::Done:
        m_status = ToolStatus::Completed;
        break;
    }
}

void LootTool::Abort()
{
    if (m_status == ToolStatus::Running || m_status == ToolStatus::Pending) {
        if (m_useNav)
            m_nav.Stop();
        else
            game::movement::StopCTM();
        if (m_lootWindowSeen)
            game::lua::Execute("CloseLoot()");
        m_status = ToolStatus::Cancelled;
    }
}

std::string LootTool::Describe() const
{
    if (m_targetName.empty())
        return m_useNav ? "Loot [nav]" : "Loot";
    char buf[128];
    snprintf(buf, sizeof(buf), "Loot \"%s\"%s",
             m_targetName.c_str(), m_useNav ? " [nav]" : "");
    return buf;
}

} // namespace bot
