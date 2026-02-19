#pragma once
// =============================================================================
// InteractTool — approach a game object/NPC and interact with it.
//
// Uses ClickToMoveInteract. Completes when player reaches interaction range.
// Generic tool for quest NPCs, mailboxes, vendors, etc.
// =============================================================================

#include "../tool.h"
#include "../../game/types.h"

#include <cstdint>

namespace bot {

class InteractTool : public ITool {
public:
    explicit InteractTool(game::GUID targetGuid);

    ToolType    GetType() const override   { return ToolType::Interact; }
    const char* GetName() const override   { return "Interact"; }
    ToolStatus  GetStatus() const override { return m_status; }

    void Start() override;
    void Tick() override;
    void Abort() override;

    std::string Describe() const override;

private:
    game::GUID  m_targetGuid;
    std::string m_targetName;
    ToolStatus  m_status = ToolStatus::Pending;
    uint64_t    m_startTick = 0;
    bool        m_interactionIssued = false;

    static constexpr uint32_t kTimeoutMs       = 15000;
    static constexpr float    kInteractRange   = 5.0f;
};

} // namespace bot
