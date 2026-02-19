#pragma once
// =============================================================================
// LootTool — approach a dead target and loot it.
//
// Uses ClickToMoveInteract to walk to the corpse.
// Auto-loots all items via Lua, completes when loot window closes.
// =============================================================================

#include "../tool.h"
#include "../../game/types.h"

#include <cstdint>

namespace bot {

class LootTool : public ITool {
public:
    explicit LootTool(game::GUID targetGuid);

    ToolType    GetType() const override   { return ToolType::Loot; }
    const char* GetName() const override   { return "Loot"; }
    ToolStatus  GetStatus() const override { return m_status; }

    void Start() override;
    void Tick() override;
    void Abort() override;

    std::string Describe() const override;

private:
    enum class Phase : uint8_t {
        Approaching,   // Walking to corpse
        Looting,       // Loot window open, picking items
        Done,
    };

    game::GUID  m_targetGuid;
    std::string m_targetName;
    ToolStatus  m_status = ToolStatus::Pending;
    Phase       m_phase  = Phase::Approaching;
    uint64_t    m_phaseStartTick = 0;
    uint64_t    m_lastInteractTick = 0;
    bool        m_lootWindowSeen = false;

    static constexpr uint32_t kApproachTimeoutMs = 15000;
    static constexpr uint32_t kLootTimeoutMs     = 5000;
};

} // namespace bot
