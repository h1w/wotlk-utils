#pragma once
// =============================================================================
// AttackTool — select a target and auto-attack it.
//
// Uses navmesh pathfinding when target is far away, switches to direct
// ClickToMoveAttack when within kNavSwitchRange.
//
// Resilient to: target deselection, RMB camera rotation (cancels CTM),
// and target movement. Re-issues attack commands within ~1s of interruption.
// =============================================================================

#include "../tool.h"
#include "../nav_helper.h"
#include "../../game/types.h"

namespace bot {

class AttackTool : public ITool {
public:
    explicit AttackTool(game::GUID targetGuid);

    ToolType    GetType() const override   { return ToolType::Attack; }
    const char* GetName() const override   { return "Attack"; }
    ToolStatus  GetStatus() const override { return m_status; }

    void Start() override;
    void Tick() override;
    void Abort() override;

    std::string Describe() const override;

    game::GUID GetTargetGuid() const { return m_targetGuid; }

private:
    void IssueCTMAttack();
    void EnsureAutoAttack();

    game::GUID  m_targetGuid;
    std::string m_targetName;
    game::Vec3  m_lastKnownPos{};
    ToolStatus  m_status = ToolStatus::Pending;
    bool        m_useNav = false;

    NavHelper   m_nav;

    // Timing
    uint64_t m_lastCTMTick       = 0;  // last time we issued ClickToMoveAttack
    uint64_t m_lastAutoAttackTick = 0; // last time we called AttackTarget() Lua
    uint64_t m_targetLostTick    = 0;  // 0 = target visible, >0 = when lost

    // Progress tracking (detect CTM cancellation by RMB)
    game::Vec3 m_lastPos{};
    uint64_t   m_lastProgressTick = 0;

    static constexpr float    kMeleeRange         = 8.0f;
    static constexpr float    kNavSwitchRange      = 15.0f;
    static constexpr float    kStuckThreshold      = 0.5f;  // yards — less than this in 1s = stalled
    static constexpr uint32_t kProgressCheckMs     = 1000;  // check movement progress every 1s
    static constexpr uint32_t kAutoAttackRecheckMs = 1500;  // re-ensure auto-attack in melee
    static constexpr uint32_t kCTMRefreshMs        = 3000;  // periodic CTM re-issue (target may move)
    static constexpr uint32_t kTargetLostGraceMs   = 5000;
};

} // namespace bot
