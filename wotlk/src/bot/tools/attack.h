#pragma once
// =============================================================================
// AttackTool — select a target and auto-attack it.
//
// Completes when target is dead. Fails if target is lost (despawned, out of
// range, etc.). Does NOT cast spells — only auto-attack via ClickToMoveAttack.
// =============================================================================

#include "../tool.h"
#include "../../game/types.h"

namespace bot {

class AttackTool : public ITool {
public:
    // Attack a specific target by GUID.
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
    void IssueAttack();

    game::GUID m_targetGuid;
    std::string m_targetName;
    ToolStatus m_status = ToolStatus::Pending;

    uint64_t m_lastAttackTick = 0;
    static constexpr uint32_t kReAttackIntervalMs = 3000;
};

} // namespace bot
