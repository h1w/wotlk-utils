#pragma once
// =============================================================================
// StrategicNavTool — executes a multi-segment WorldGraph route.
//
// Takes a route plan (sequence of RouteSegments from WorldGraph::PlanRoute)
// and executes each segment:
//   Walk     → MoveToTool (navmesh pathfinding)
//   Flight   → InteractTool with flight master + WaitTool for taxi
//   Teleport → InteractTool with portal
//   Boat     → MoveToTool to dock + WaitTool for transport
//
// Each segment completes before the next starts.
// =============================================================================

#include "../tool.h"
#include "../../navigation/world_graph.h"

#include <vector>
#include <cstdint>

namespace bot {

class StrategicNavTool : public ITool {
public:
    // Construct from a pre-planned route.
    explicit StrategicNavTool(std::vector<nav::RouteSegment> route);

    // Convenience: plan route from current position to target node.
    // Returns nullptr if graph not loaded or no route found.
    static std::unique_ptr<StrategicNavTool> CreateFromTarget(uint32_t targetNodeId);

    ToolType    GetType() const override   { return ToolType::StrategicNav; }
    const char* GetName() const override   { return "Strategic Nav"; }
    ToolStatus  GetStatus() const override { return m_status; }

    void Start() override;
    void Tick() override;
    void Abort() override;

    std::string Describe() const override;

    // Queries
    size_t GetSegmentCount() const       { return m_route.size(); }
    size_t GetCurrentSegment() const     { return m_currentSegment; }
    const std::vector<nav::RouteSegment>& GetRoute() const { return m_route; }

private:
    std::vector<nav::RouteSegment> m_route;
    size_t     m_currentSegment = 0;
    ToolStatus m_status = ToolStatus::Pending;
    ToolPtr    m_activeTool;        // current sub-tool for this segment

    void StartCurrentSegment();
    void AdvanceToNextSegment();
};

} // namespace bot
