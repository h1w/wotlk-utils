#pragma once
#include <vector>

class dtNavMesh;
class dtNavMeshQuery;

namespace mapedit {

struct PathPoint {
    float x = 0, y = 0, z = 0;
};

struct PathResult {
    bool success = false;
    bool partial = false;
    std::vector<PathPoint> waypoints;
    float totalDistance = 0.0f;
};

class MapPathfinder {
public:
    PathResult FindPath(dtNavMeshQuery* query,
                        float startX, float startY, float startZ,
                        float endX, float endY, float endZ);

private:
    static constexpr int kMaxPathPolygons = 2048;
    static constexpr int kMaxStraightPath = 512;
    static constexpr float kSearchExtent = 50.0f;
    static constexpr float kSearchExtentY = 100.0f;
};

} // namespace mapedit
