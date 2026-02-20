#include "strategic_nav.h"
#include "move_to.h"
#include "wait.h"
#include "../../game/game.h"
#include "../../game/world.h"
#include "../../navigation/world_graph.h"

#include <cstdio>

#include <glog/logging.h>

namespace bot {

StrategicNavTool::StrategicNavTool(std::vector<nav::RouteSegment> route)
    : m_route(std::move(route))
{
}

std::unique_ptr<StrategicNavTool> StrategicNavTool::CreateFromTarget(uint32_t targetNodeId) {
    auto& graph = nav::WorldGraph::Instance();
    if (!graph.IsLoaded())
        return nullptr;

    auto player = game::GetLocalPlayer();
    if (!player || !game::world::IsInGame())
        return nullptr;

    uint32_t mapId = game::world::GetMapId();
    game::Vec3 pos = player->GetPosition();

    auto route = graph.PlanRouteFromPos(mapId, pos.x, pos.y, targetNodeId);
    if (route.empty())
        return nullptr;

    return std::make_unique<StrategicNavTool>(std::move(route));
}

void StrategicNavTool::Start() {
    if (m_route.empty()) {
        m_status = ToolStatus::Failed;
        return;
    }

    if (!game::world::IsInGame()) {
        m_status = ToolStatus::Failed;
        return;
    }

    m_currentSegment = 0;
    m_status = ToolStatus::Running;

    LOG(INFO) << "[StrategicNav] Starting route with " << m_route.size() << " segments";
    StartCurrentSegment();
}

void StrategicNavTool::Tick() {
    if (m_status != ToolStatus::Running)
        return;

    if (!m_activeTool) {
        m_status = ToolStatus::Failed;
        return;
    }

    m_activeTool->Tick();

    switch (m_activeTool->GetStatus()) {
    case ToolStatus::Completed:
        AdvanceToNextSegment();
        break;
    case ToolStatus::Failed:
        LOG(WARNING) << "[StrategicNav] Segment " << m_currentSegment
                     << " failed: " << m_activeTool->Describe();
        m_status = ToolStatus::Failed;
        break;
    case ToolStatus::Cancelled:
        m_status = ToolStatus::Cancelled;
        break;
    default:
        break; // still running
    }
}

void StrategicNavTool::Abort() {
    if (m_status == ToolStatus::Running || m_status == ToolStatus::Pending) {
        if (m_activeTool)
            m_activeTool->Abort();
        m_status = ToolStatus::Cancelled;
    }
}

void StrategicNavTool::StartCurrentSegment() {
    if (m_currentSegment >= m_route.size()) {
        m_status = ToolStatus::Completed;
        return;
    }

    const auto& seg = m_route[m_currentSegment];

    const char* typeStr = "?";
    switch (seg.edgeType) {
    case nav::EdgeType::Walk:     typeStr = "Walk"; break;
    case nav::EdgeType::Flight:   typeStr = "Flight"; break;
    case nav::EdgeType::Teleport: typeStr = "Teleport"; break;
    case nav::EdgeType::Boat:     typeStr = "Boat"; break;
    }

    LOG(INFO) << "[StrategicNav] Starting segment " << m_currentSegment
              << "/" << m_route.size() << ": " << typeStr
              << " (node " << seg.fromNodeId << " -> " << seg.toNodeId << ")";

    switch (seg.edgeType) {
    case nav::EdgeType::Walk:
        // Navigate via navmesh
        m_activeTool = std::make_unique<MoveToTool>(seg.toPos);
        m_activeTool->Start();
        break;

    case nav::EdgeType::Flight:
        // TODO: Need to interact with flight master NPC, select destination.
        // For now, walk to the flight master position (the interaction
        // logic will be added when NPC interaction is more developed).
        LOG(WARNING) << "[StrategicNav] Flight segment — walking to flight master "
                     << "(auto-flight not yet implemented)";
        m_activeTool = std::make_unique<MoveToTool>(seg.toPos);
        m_activeTool->Start();
        break;

    case nav::EdgeType::Teleport:
        // TODO: Walk to portal and use it.
        LOG(WARNING) << "[StrategicNav] Teleport segment — walking to portal "
                     << "(auto-teleport not yet implemented)";
        m_activeTool = std::make_unique<MoveToTool>(seg.toPos);
        m_activeTool->Start();
        break;

    case nav::EdgeType::Boat:
        // TODO: Walk to dock, wait for boat, ride it.
        LOG(WARNING) << "[StrategicNav] Boat segment — walking to dock "
                     << "(auto-boat not yet implemented)";
        m_activeTool = std::make_unique<MoveToTool>(seg.toPos);
        m_activeTool->Start();
        break;
    }
}

void StrategicNavTool::AdvanceToNextSegment() {
    m_currentSegment++;
    m_activeTool.reset();

    if (m_currentSegment >= m_route.size()) {
        LOG(INFO) << "[StrategicNav] Route complete (" << m_route.size() << " segments)";
        m_status = ToolStatus::Completed;
        return;
    }

    StartCurrentSegment();
}

std::string StrategicNavTool::Describe() const {
    char buf[128];
    snprintf(buf, sizeof(buf), "Strategic Nav [%zu/%zu]%s",
             m_currentSegment + 1, m_route.size(),
             m_activeTool ? (" — " + m_activeTool->Describe()).c_str() : "");
    return buf;
}

} // namespace bot
