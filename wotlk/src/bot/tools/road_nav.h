#pragma once
// =============================================================================
// RoadNavTool — three-phase road following executor.
//
// Phase 1 (Approach):   NavHelper walks from current position to road entry node
// Phase 2 (RoadFollow): Segment-by-segment waypoint following along road nodes
// Phase 3 (Departure):  NavHelper walks from road exit node to final destination
//
// Danger avoidance: checks each road node for nearby threats before targeting it.
// Skips dangerous nodes and falls back to pure navmesh if too many skips.
// =============================================================================

#include "../tool.h"
#include "../nav_helper.h"
#include "../../game/types.h"
#include "../../navigation/road_graph.h"

namespace bot {

class RoadNavTool : public ITool {
public:
    RoadNavTool(const game::Vec3& start, const game::Vec3& end,
                nav::RoadPlan plan);

    ToolType    GetType() const override   { return ToolType::RoadNav; }
    const char* GetName() const override   { return "Road Nav"; }
    ToolStatus  GetStatus() const override { return m_status; }

    void Start() override;
    void Tick() override;
    void Abort() override;

    std::string Describe() const override;

    // Expose nav path for radar visualization
    const std::vector<game::Vec3>& GetNavWaypoints() const { return m_nav.GetWaypoints(); }
    size_t GetNavCurrentIndex() const { return m_nav.GetCurrentWaypointIndex(); }
    size_t GetRoadIndex() const { return m_roadIndex; }
    size_t GetRoadNodeCount() const { return m_plan.roadPath.size(); }
    const game::Vec3& GetDestination() const { return m_end; }
    const std::vector<game::Vec3>& GetDetourWaypoints() const { return m_nav.GetDetourWaypoints(); }

private:
    enum class Phase {
        Approach,     // navmesh: current pos → entry road node
        RoadFollow,   // segment-by-segment: road node → road node
        Departure,    // navmesh: exit road node → final destination
        Done,
        Failed
    };

    Phase      m_phase = Phase::Approach;
    ToolStatus m_status = ToolStatus::Pending;
    game::Vec3 m_start;
    game::Vec3 m_end;
    nav::RoadPlan m_plan;

    size_t m_roadIndex = 0;            // current index in m_plan.roadPath
    int    m_skippedConsecutive = 0;   // danger/unreachable skip counter
    NavHelper m_nav;

    static constexpr float kRoadArrivalThreshold = 5.0f;   // yards (wider than default 2.5)
    static constexpr int   kMaxConsecutiveSkips   = 5;      // abandon road after 5 skips
    static constexpr float kThreatScanRadius      = 45.0f;  // yards around road node
    static constexpr float kThreatMargin          = 5.0f;   // extra margin on aggro radius

    void StartNextRoadSegment();
    void SkipCurrentRoadNode();
    bool IsNodeNearThreat(const game::Vec3& nodePos) const;
    game::Vec3 GetRoadNodePos(size_t pathIndex) const;
};

} // namespace bot
