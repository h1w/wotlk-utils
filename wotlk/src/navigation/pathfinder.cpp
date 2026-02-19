#include "pathfinder.h"
#include "nav_mesh.h"

#include <DetourNavMesh.h>
#include <DetourNavMeshQuery.h>
#include <DetourStatus.h>
#include <DetourCommon.h>

#include <glog/logging.h>

#include <cstring>

namespace nav {

Pathfinder& Pathfinder::Instance() {
    static Pathfinder s_instance;
    return s_instance;
}

Pathfinder::Pathfinder() {
    ResetAreaCosts();
}

void Pathfinder::ResetAreaCosts() {
    for (int i = 0; i < 64; ++i)
        m_areaCosts[i] = 1.0f;
}

void Pathfinder::SetAreaCost(uint8_t areaId, float cost) {
    if (areaId < 64)
        m_areaCosts[areaId] = cost;
}

PathResult Pathfinder::FindPath(const game::Vec3& start, const game::Vec3& end) {
    PathResult result;

    auto& navMesh = NavMesh::Instance();
    if (!navMesh.IsReady()) {
        LOG(WARNING) << "[Nav] FindPath: navmesh not ready";
        return result;
    }

    dtNavMeshQuery* query = navMesh.GetQuery();
    if (!query) return result;

    // Build filter with area costs
    dtQueryFilter filter;
    filter.setIncludeFlags(0xFFFF); // include all polygon types
    filter.setExcludeFlags(0);
    for (int i = 0; i < 64; ++i)
        filter.setAreaCost(i, m_areaCosts[i]);

    // TrinityCore mmaps: Detour[0]=WowY, Detour[1]=WowZ (up), Detour[2]=WowX
    float startPos[3] = { start.y, start.z, start.x };
    float endPos[3]   = { end.y,   end.z,   end.x };
    float extents[3]  = { kSearchExtentXZ, kSearchExtentY, kSearchExtentXZ };

    // Find nearest polygons
    dtPolyRef startRef = 0, endRef = 0;
    float closestStart[3], closestEnd[3];

    dtStatus status = query->findNearestPoly(startPos, extents, &filter, &startRef, closestStart);
    if (dtStatusFailed(status) || startRef == 0) {
        LOG(WARNING) << "[Nav] FindPath: no start poly near ("
                     << start.x << ", " << start.y << ", " << start.z << ")";
        return result;
    }

    status = query->findNearestPoly(endPos, extents, &filter, &endRef, closestEnd);
    if (dtStatusFailed(status) || endRef == 0) {
        LOG(WARNING) << "[Nav] FindPath: no end poly near ("
                     << end.x << ", " << end.y << ", " << end.z << ")";
        return result;
    }

    // A* through polygon graph
    dtPolyRef pathPolys[kMaxPathPolygons];
    int pathCount = 0;

    status = query->findPath(startRef, endRef, closestStart, closestEnd,
                             &filter, pathPolys, &pathCount, kMaxPathPolygons);

    if (dtStatusFailed(status) || pathCount == 0) {
        LOG(WARNING) << "[Nav] FindPath: findPath failed, status=0x" << std::hex << status;
        return result;
    }

    // Check if path is partial (didn't reach destination)
    if (dtStatusDetail(status, DT_PARTIAL_RESULT))
        result.partial = true;

    // String-pull to straight path (XYZ waypoints)
    float straightPath[kMaxStraightPath * 3];
    unsigned char straightPathFlags[kMaxStraightPath];
    dtPolyRef straightPathPolys[kMaxStraightPath];
    int straightPathCount = 0;

    status = query->findStraightPath(closestStart, closestEnd,
                                     pathPolys, pathCount,
                                     straightPath, straightPathFlags, straightPathPolys,
                                     &straightPathCount, kMaxStraightPath,
                                     DT_STRAIGHTPATH_ALL_CROSSINGS);

    if (dtStatusFailed(status) || straightPathCount == 0) {
        LOG(WARNING) << "[Nav] FindPath: findStraightPath failed";
        return result;
    }

    // Convert back: WoW X = Detour[2], WoW Y = Detour[0], WoW Z = Detour[1]
    result.waypoints.reserve(straightPathCount);
    for (int i = 0; i < straightPathCount; ++i) {
        game::Vec3 wp;
        wp.x = straightPath[i * 3 + 2]; // Detour[2] = WoW X
        wp.y = straightPath[i * 3 + 0]; // Detour[0] = WoW Y
        wp.z = straightPath[i * 3 + 1] + kZOffset; // Detour[1] = WoW Z
        result.waypoints.push_back(wp);
    }

    result.success = true;

    LOG(INFO) << "[Nav] Path found: " << result.waypoints.size() << " waypoints"
              << (result.partial ? " (PARTIAL)" : "");

    return result;
}

} // namespace nav
