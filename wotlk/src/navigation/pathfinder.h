#pragma once
// =============================================================================
// Pathfinder — builds paths using Detour navmesh queries.
//
// Wraps dtNavMeshQuery::findPath + findStraightPath with game::Vec3 interface.
// =============================================================================

#include "../game/types.h"

#include <vector>
#include <cstdint>

class dtQueryFilter;
struct dtNavMeshQuery;
typedef unsigned int dtPolyRef;

namespace nav {

struct PathResult {
    std::vector<game::Vec3> waypoints;
    bool success      = false;
    bool partial       = false;  // path found but doesn't reach destination
    bool throughDanger = false;  // path passes through a marked danger zone
};

struct DangerZone {
    float x, y, z;   // center (WoW coords)
    float radius;     // aggro radius in yards
};

class Pathfinder {
public:
    static Pathfinder& Instance();

    // Find path from start to end on the currently loaded navmesh
    PathResult FindPath(const game::Vec3& start, const game::Vec3& end);

    // Find path that avoids danger zones via single-pass area cost marking.
    //   1. Marks danger polys with area 63 (cost 50x) — A* steers around them.
    //   2. Post-validates result against actual marked polys.
    //   3. Sets throughDanger=true if A* was forced through danger.
    // Uses permissive filter for findNearestPoly, danger-aware for findPath.
    // Restores original poly areas after query (RAII PolyAreaGuard).
    PathResult FindPathAvoiding(const game::Vec3& start, const game::Vec3& end,
                                const std::vector<DangerZone>& dangers);

    // Area cost configuration
    void SetAreaCost(uint8_t areaId, float cost);
    void ResetAreaCosts();

private:
    Pathfinder();

    static constexpr int   kMaxPathPolygons  = 256;
    static constexpr int   kMaxStraightPath  = 128;
    static constexpr float kSearchExtentXZ   = 10.0f;  // horizontal search radius (yards)
    static constexpr float kSearchExtentY    = 20.0f;  // vertical search radius (yards)
    static constexpr float kLargeExtentXZ    = 30.0f;  // expanded search for fallback
    static constexpr float kLargeExtentY     = 40.0f;
    static constexpr float kZOffset          = 0.5f;   // offset to prevent ground clipping

    // Danger zone constants
    static constexpr uint8_t  kDangerAreaId      = 63;     // area ID for cost marking
    static constexpr float    kDangerAreaCost    = 500.0f; // cost multiplier (500x = very strong avoidance)
    static constexpr float    kAggroMarginMult   = 1.15f;  // multiplicative margin (15%)
    static constexpr float    kAggroMarginAdd    = 3.0f;   // additive margin (yards)
    static constexpr float    kAggroQueryExtentY = 10.0f;  // Y extent for queryPolygons
    static constexpr int      kMaxDangerPolys    = 256;    // max polys per queryPolygons call

    // Area cost storage (applied to dtQueryFilter each query)
    float m_areaCosts[64];

    // Internal: build filter with current area costs
    void BuildFilter(dtQueryFilter& filter) const;

    // Internal: core pathfinding with a given filter
    PathResult FindPathWithFilter(const game::Vec3& start, const game::Vec3& end,
                                  const dtQueryFilter& filter);

    // Internal: string-pull poly path into straight waypoints
    PathResult BuildStraightPath(dtNavMeshQuery* query,
                                 const float* closestStart, const float* closestEnd,
                                 const dtPolyRef* pathPolys, int pathCount,
                                 bool throughDanger, bool partial);

    // Internal: push waypoints away from aggro circles that the string-pulled
    // path clips through. Returns true if any waypoints were nudged.
    static bool NudgeWaypointsFromDangers(PathResult& result,
                                          const std::vector<DangerZone>& dangers,
                                          float marginMult, float marginAdd);
};

} // namespace nav
