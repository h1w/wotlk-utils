#pragma once
// =============================================================================
// LootTool — approach a dead target and loot it.
//
// Uses navmesh pathfinding when far from the corpse, switches to direct
// ClickToMoveInteract when within kNavSwitchRange. Falls back to direct CTM
// if navmesh is unavailable.
// =============================================================================

#include "../tool.h"
#include "../nav_helper.h"
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
    game::Vec3  m_targetPos{};     // saved at Start() — survives GUID invalidation
    ToolStatus  m_status = ToolStatus::Pending;
    Phase       m_phase  = Phase::Approaching;
    bool        m_useNav = false;

    NavHelper   m_nav;

    uint64_t    m_phaseStartTick = 0;
    uint64_t    m_lastInteractTick = 0;
    uint64_t    m_lastReissueTick = 0;
    uint64_t    m_firstInRangeTick = 0;  // when we first got in loot range (0 = not yet)
    bool        m_lootWindowSeen = false;

    static constexpr float    kNavSwitchRange    = 10.0f;
    static constexpr uint32_t kApproachTimeoutMs = 15000;
    static constexpr uint32_t kLootTimeoutMs     = 5000;
    static constexpr uint32_t kNoLootTimeoutMs   = 3000;
};

} // namespace bot
