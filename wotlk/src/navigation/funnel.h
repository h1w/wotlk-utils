#pragma once
// =============================================================================
// Funnel — custom string-pulling with portal shrinking.
//
// Reimplements Detour's findStraightPath using only public dtNavMesh API.
// Contracts each portal (shared edge) inward by a configurable margin before
// running the funnel, keeping the path away from polygon edges.
// =============================================================================

class dtNavMesh;
typedef unsigned int dtPolyRef;

namespace nav {

struct Portal {
    float left[3];
    float right[3];
    bool isOffMesh;
    dtPolyRef polyRef;
};

// Custom funnel (string-pull) with portal shrinking.
// All positions in Detour coordinates. Returns false if portal extraction
// fails (caller should fall back to dtNavMeshQuery::findStraightPath).
bool FunnelStraightPath(const dtNavMesh* mesh,
                        const float* startPos,
                        const float* endPos,
                        const dtPolyRef* pathPolys,
                        int pathCount,
                        float margin,
                        float* outPoints,
                        int& outCount,
                        int maxPoints);

} // namespace nav
