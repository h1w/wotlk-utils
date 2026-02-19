#include "loot.h"
#include "../../game/game.h"
#include "../../game/movement.h"
#include "../../game/world.h"
#include "../../game/object_manager.h"
#include "../../game/lua_bridge.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdio>

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

    // Select the target first (InteractUnit works on "target")
    game::SelectTarget(m_targetGuid);

    // Walk to corpse first, then loot when in range
    game::Vec3 targetPos = target.GetPosition();
    game::movement::ClickToMove(targetPos);

    m_phase = Phase::Approaching;
    m_phaseStartTick = GetTickCount64();
    m_status = ToolStatus::Running;
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
            m_status = ToolStatus::Failed;
            return;
        }

        // Check if loot window is already open
        int numItems = game::lua::GetInt("GetNumLootItems()");
        if (numItems > 0) {
            m_lootWindowSeen = true;
            m_phase = Phase::Looting;
            m_phaseStartTick = now;

            // Auto-loot all items
            for (int i = 1; i <= numItems; ++i)
                game::lua::Executef("LootSlot(%d)", i);
            return;
        }

        // If in range but no loot window yet, interact via Lua (once per second)
        auto player = game::GetLocalPlayer();
        uintptr_t ptr = game::objmgr::GetObjectPtr(m_targetGuid);
        if (player && ptr != 0) {
            game::Unit target(ptr);
            float dist = player->GetPosition().DistanceTo(target.GetPosition());
            if (dist <= kLootRange && (now - m_lastInteractTick > 1000)) {
                game::movement::StopCTM();
                game::SelectTarget(m_targetGuid);
                game::lua::Execute("InteractUnit(\"target\")");
                m_lastInteractTick = now;
            }
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
        game::movement::StopCTM();
        if (m_lootWindowSeen)
            game::lua::Execute("CloseLoot()");
        m_status = ToolStatus::Cancelled;
    }
}

std::string LootTool::Describe() const
{
    if (m_targetName.empty())
        return "Loot";
    char buf[128];
    snprintf(buf, sizeof(buf), "Loot \"%s\"", m_targetName.c_str());
    return buf;
}

} // namespace bot
