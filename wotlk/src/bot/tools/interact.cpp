#include "interact.h"
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

InteractTool::InteractTool(game::GUID targetGuid)
    : m_targetGuid(targetGuid)
{
}

void InteractTool::Start()
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

    game::WowObject obj(ptr);
    m_targetName = obj.GetName();
    m_targetPos = obj.GetPosition();

    game::SelectTarget(m_targetGuid);

    m_startTick = GetTickCount64();
    m_status = ToolStatus::Running;
    m_useNav = false;

    // If far from target, use navmesh to approach
    auto player = game::GetLocalPlayer();
    float dist = player ? player->GetPosition().DistanceTo(m_targetPos) : 0.0f;

    if (dist > kNavSwitchRange && m_nav.StartNavTo(m_targetPos)) {
        m_useNav = true;
        LOG(INFO) << "[InteractTool] Using navmesh to approach target (" << dist << " yards)";
    } else {
        // Close enough or no navmesh — direct CTM interact
        game::movement::ClickToMoveInteract(m_targetGuid, m_targetPos);
    }
}

void InteractTool::Tick()
{
    if (m_status != ToolStatus::Running)
        return;

    uint64_t now = GetTickCount64();

    // Timeout
    if (now - m_startTick > kTimeoutMs) {
        if (m_useNav)
            m_nav.Stop();
        m_status = ToolStatus::Failed;
        return;
    }

    // Check if target still exists
    uintptr_t ptr = game::objmgr::GetObjectPtr(m_targetGuid);
    if (ptr == 0) {
        if (m_useNav)
            m_nav.Stop();
        // Object despawned — interaction may have consumed it (herb, ore, etc.)
        if (m_interactionIssued)
            m_status = ToolStatus::Completed;
        else
            m_status = ToolStatus::Failed;
        return;
    }

    game::WowObject obj(ptr);
    m_targetPos = obj.GetPosition();

    auto player = game::GetLocalPlayer();
    if (!player) {
        m_status = ToolStatus::Failed;
        return;
    }

    float dist = player->GetPosition().DistanceTo(m_targetPos);

    // Nav approach mode
    if (m_useNav) {
        if (dist <= kNavSwitchRange) {
            // Close enough — stop nav, switch to direct CTM interact
            m_nav.Stop();
            m_useNav = false;
            LOG(INFO) << "[InteractTool] Within " << kNavSwitchRange << "y, switching to direct interact";
            game::SelectTarget(m_targetGuid);
            game::movement::ClickToMoveInteract(m_targetGuid, m_targetPos);
            return;
        }

        auto navStatus = m_nav.Tick();
        if (navStatus == NavHelper::Status::Arrived || navStatus == NavHelper::Status::Failed) {
            m_useNav = false;
            if (navStatus == NavHelper::Status::Failed)
                LOG(WARNING) << "[InteractTool] Nav failed, switching to direct interact";
            game::SelectTarget(m_targetGuid);
            game::movement::ClickToMoveInteract(m_targetGuid, m_targetPos);
        }
        return;
    }

    // Direct mode — check distance for interaction
    if (dist <= kInteractRange) {
        if (!m_interactionIssued) {
            game::SelectTarget(m_targetGuid);
            game::lua::Execute("InteractUnit(\"target\")");
            m_interactionIssued = true;
        } else {
            m_status = ToolStatus::Completed;
        }
    }
}

void InteractTool::Abort()
{
    if (m_status == ToolStatus::Running || m_status == ToolStatus::Pending) {
        if (m_useNav)
            m_nav.Stop();
        else
            game::movement::StopCTM();
        m_status = ToolStatus::Cancelled;
    }
}

std::string InteractTool::Describe() const
{
    if (m_targetName.empty())
        return m_useNav ? "Interact [nav]" : "Interact";
    char buf[128];
    snprintf(buf, sizeof(buf), "Interact \"%s\"%s",
             m_targetName.c_str(), m_useNav ? " [nav]" : "");
    return buf;
}

} // namespace bot
