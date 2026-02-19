#include "interact.h"
#include "../../game/game.h"
#include "../../game/movement.h"
#include "../../game/world.h"
#include "../../game/object_manager.h"
#include "../../game/lua_bridge.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdio>

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

    // Select and walk to object
    game::SelectTarget(m_targetGuid);
    game::Vec3 targetPos = obj.GetPosition();
    game::movement::ClickToMoveInteract(m_targetGuid, targetPos);

    m_startTick = GetTickCount64();
    m_status = ToolStatus::Running;
}

void InteractTool::Tick()
{
    if (m_status != ToolStatus::Running)
        return;

    uint64_t now = GetTickCount64();

    // Timeout
    if (now - m_startTick > kTimeoutMs) {
        m_status = ToolStatus::Failed;
        return;
    }

    // Check if target still exists
    uintptr_t ptr = game::objmgr::GetObjectPtr(m_targetGuid);
    if (ptr == 0) {
        // Object despawned — interaction may have consumed it (herb, ore, etc.)
        if (m_interactionIssued)
            m_status = ToolStatus::Completed;
        else
            m_status = ToolStatus::Failed;
        return;
    }

    // Check distance
    auto player = game::GetLocalPlayer();
    if (!player) {
        m_status = ToolStatus::Failed;
        return;
    }

    game::WowObject obj(ptr);
    float dist = player->GetPosition().DistanceTo(obj.GetPosition());

    if (dist <= kInteractRange) {
        if (!m_interactionIssued) {
            // In range — use Lua InteractUnit for reliable interaction
            game::SelectTarget(m_targetGuid);
            game::lua::Execute("InteractUnit(\"target\")");
            m_interactionIssued = true;
        } else {
            // Already interacted and still in range — done
            m_status = ToolStatus::Completed;
        }
    }
}

void InteractTool::Abort()
{
    if (m_status == ToolStatus::Running || m_status == ToolStatus::Pending) {
        game::movement::StopCTM();
        m_status = ToolStatus::Cancelled;
    }
}

std::string InteractTool::Describe() const
{
    if (m_targetName.empty())
        return "Interact";
    char buf[128];
    snprintf(buf, sizeof(buf), "Interact \"%s\"", m_targetName.c_str());
    return buf;
}

} // namespace bot
