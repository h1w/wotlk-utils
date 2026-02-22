#include "pathfinder.h"

#include <DetourNavMesh.h>
#include <DetourNavMeshQuery.h>
#include <DetourStatus.h>

#include <glog/logging.h>
#include <cmath>

namespace mapedit {

PathResult MapPathfinder::FindPath(dtNavMeshQuery* query,
                                    float startX, float startY, float startZ,
                                    float endX, float endY, float endZ) {
    PathResult result;
    if (!query) return result;

    // WoW -> Detour: d[0]=wowY, d[1]=wowZ, d[2]=wowX
    float startPos[3] = { startY, startZ, startX };
    float endPos[3]   = { endY,   endZ,   endX };
    float extents[3]  = { kSearchExtent, kSearchExtentY, kSearchExtent };

    dtQueryFilter filter;
    filter.setIncludeFlags(0xFFFF);
    filter.setExcludeFlags(0);

    dtPolyRef startRef = 0, endRef = 0;
    float closestStart[3], closestEnd[3];

    dtStatus status = query->findNearestPoly(startPos, extents, &filter, &startRef, closestStart);
    if (dtStatusFailed(status) || startRef == 0) {
        LOG(WARNING) << "[Pathfinder] No start poly near (" << startX << ", " << startY << ")";
        return result;
    }

    status = query->findNearestPoly(endPos, extents, &filter, &endRef, closestEnd);
    if (dtStatusFailed(status) || endRef == 0) {
        // Retry with larger extents
        float bigExtents[3] = { 200.0f, 500.0f, 200.0f };
        status = query->findNearestPoly(endPos, bigExtents, &filter, &endRef, closestEnd);
        if (dtStatusFailed(status) || endRef == 0) {
            LOG(WARNING) << "[Pathfinder] No end poly near (" << endX << ", " << endY << ")";
            return result;
        }
    }

    dtPolyRef pathPolys[kMaxPathPolygons];
    int pathCount = 0;

    status = query->findPath(startRef, endRef, closestStart, closestEnd,
                             &filter, pathPolys, &pathCount, kMaxPathPolygons);
    if (dtStatusFailed(status) || pathCount == 0)
        return result;

    if (dtStatusDetail(status, DT_PARTIAL_RESULT))
        result.partial = true;

    float straightPath[kMaxStraightPath * 3];
    unsigned char straightPathFlags[kMaxStraightPath];
    dtPolyRef straightPathPolys[kMaxStraightPath];
    int straightPathCount = 0;

    status = query->findStraightPath(closestStart, closestEnd,
                                     pathPolys, pathCount,
                                     straightPath, straightPathFlags, straightPathPolys,
                                     &straightPathCount, kMaxStraightPath,
                                     DT_STRAIGHTPATH_ALL_CROSSINGS);
    if (dtStatusFailed(status) || straightPathCount == 0)
        return result;

    result.waypoints.reserve(straightPathCount);
    for (int i = 0; i < straightPathCount; ++i) {
        PathPoint wp;
        wp.x = straightPath[i * 3 + 2]; // WoW X = Detour[2]
        wp.y = straightPath[i * 3 + 0]; // WoW Y = Detour[0]
        wp.z = straightPath[i * 3 + 1]; // WoW Z = Detour[1]
        result.waypoints.push_back(wp);
    }

    // Calculate total distance
    result.totalDistance = 0;
    for (size_t i = 1; i < result.waypoints.size(); ++i) {
        float dx = result.waypoints[i].x - result.waypoints[i-1].x;
        float dy = result.waypoints[i].y - result.waypoints[i-1].y;
        float dz = result.waypoints[i].z - result.waypoints[i-1].z;
        result.totalDistance += std::sqrt(dx*dx + dy*dy + dz*dz);
    }

    result.success = true;
    return result;
}

} // namespace mapedit
