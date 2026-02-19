#pragma once
// =============================================================================
// InteractTool — approach a game object/NPC and interact with it.
//
// Uses navmesh pathfinding when far from the target, switches to direct
// ClickToMoveInteract when within kNavSwitchRange. Falls back to direct CTM
// if navmesh is unavailable.
// =============================================================================

#include "../tool.h"
#include "../nav_helper.h"
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
    game::Vec3  m_targetPos{};
    ToolStatus  m_status = ToolStatus::Pending;
    bool        m_useNav = false;
    bool        m_interactionIssued = false;

    NavHelper   m_nav;

    uint64_t    m_startTick = 0;

    static constexpr float    kNavSwitchRange = 10.0f;
    static constexpr uint32_t kTimeoutMs      = 15000;
    static constexpr float    kInteractRange  = 5.0f;
};

} // namespace bot
