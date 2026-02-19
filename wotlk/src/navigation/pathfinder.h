#pragma once
// =============================================================================
// Pathfinder — builds paths using Detour navmesh queries.
//
// Wraps dtNavMeshQuery::findPath + findStraightPath with game::Vec3 interface.
// =============================================================================

#include "../game/types.h"

#include <vector>
#include <cstdint>

namespace nav {

struct PathResult {
    std::vector<game::Vec3> waypoints;
    bool success = false;
    bool partial = false;  // path found but doesn't reach destination
};

class Pathfinder {
public:
    static Pathfinder& Instance();

    // Find path from start to end on the currently loaded navmesh
    PathResult FindPath(const game::Vec3& start, const game::Vec3& end);

    // Area cost configuration
    void SetAreaCost(uint8_t areaId, float cost);
    void ResetAreaCosts();

private:
    Pathfinder();

    static constexpr int   kMaxPathPolygons  = 256;
    static constexpr int   kMaxStraightPath  = 128;
    static constexpr float kSearchExtentXZ   = 5.0f;  // horizontal search radius
    static constexpr float kSearchExtentY    = 10.0f;  // vertical search radius
    static constexpr float kZOffset          = 0.5f;  // offset to prevent ground clipping

    // Area cost storage (applied to dtQueryFilter each query)
    float m_areaCosts[64];
};

} // namespace nav
