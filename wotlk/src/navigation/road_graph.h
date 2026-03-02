#pragma once
// =============================================================================
// RoadGraph — regional road network for medium-distance navigation.
//
// Loads a verified road graph (extracted from ADT textures, manually cleaned)
// as an independent navigation layer between strategic WorldGraph and tactical
// Detour navmesh. A* search on the road graph, spatial grid for nearest-node
// queries, and bridge computation to connect WorldGraph POIs to road nodes.
// =============================================================================

#include "../game/types.h"

#include <cstdint>
#include <string>
#include <vector>
#include <unordered_map>

namespace nav {

// Forward declaration
struct WorldNode;

// ---- Road Node (graph vertex) ----

struct RoadNode {
    uint32_t id     = 0;
    float    x      = 0.f;
    float    y      = 0.f;
    float    z      = 0.f;     // real terrain height
    uint32_t mapId  = 0;
};

// ---- Road Edge (graph edge) ----

struct RoadEdge {
    uint32_t from   = 0;
    uint32_t to     = 0;
    float    cost   = 0.f;     // Euclidean distance in yards
    // All edges are bidirectional walk — no type/bidir fields needed
};

// ---- Spatial Grid (fast nearest-node lookup) ----

struct SpatialGrid {
    static constexpr float kCellSize = 100.0f;  // 100-yard cells

    struct CellKey {
        int32_t cx = 0;
        int32_t cy = 0;
        bool operator==(const CellKey& o) const { return cx == o.cx && cy == o.cy; }
    };

    struct CellKeyHash {
        size_t operator()(const CellKey& k) const {
            return std::hash<int64_t>()(((int64_t)k.cx << 32) | (uint32_t)k.cy);
        }
    };

    std::unordered_map<CellKey, std::vector<uint32_t>, CellKeyHash> cells;

    void Clear();
    void Insert(uint32_t nodeId, float x, float y);

    // Search 3x3 cells around (x,y), return closest node within maxRadius.
    // Returns 0 if no node found.
    uint32_t FindNearest(float x, float y, float z, float maxRadius,
                         const std::vector<RoadNode>& nodes,
                         const std::unordered_map<uint32_t, size_t>& nodeIndex) const;
};

// ---- Bridge (WorldGraph node ↔ nearest road node) ----

struct Bridge {
    uint32_t worldNodeId = 0;
    uint32_t roadNodeId  = 0;
    float    distance    = 0.f;   // 3D distance between them
};

// ---- Road Path Plan (result of PlanRoadPath) ----

struct RoadPlan {
    bool                  useRoad      = false;
    uint32_t              entryNodeId  = 0;
    uint32_t              exitNodeId   = 0;
    std::vector<uint32_t> roadPath;          // ordered road node IDs (entry → exit)
    float                 approachDist = 0.f; // start → entry
    float                 roadDist     = 0.f; // total road edge distance
    float                 departureDist= 0.f; // exit → end
    float                 totalDist    = 0.f; // sum of all three
    float                 directDist   = 0.f; // straight-line start → end
};

// ---- RoadGraph (singleton) ----

class RoadGraph {
public:
    static RoadGraph& Instance();

    // Loading
    bool LoadFromFile(uint32_t mapId, const char* jsonPath);
    void Clear();
    bool IsLoaded(uint32_t mapId) const;

    // Queries
    const RoadNode* FindNearestNode(uint32_t mapId, float x, float y, float z,
                                     float maxRadius = 200.0f) const;

    // A* pathfinding — returns ordered node IDs, empty if no path
    std::vector<uint32_t> FindPath(uint32_t fromNodeId, uint32_t toNodeId) const;

    // Get node by ID
    const RoadNode* GetNode(uint32_t id) const;

    // Stats
    uint32_t GetNodeCount(uint32_t mapId) const;
    uint32_t GetEdgeCount(uint32_t mapId) const;

    // Road path planning
    RoadPlan PlanRoadPath(const game::Vec3& start, const game::Vec3& end,
                          uint32_t mapId) const;

    // Bridge computation (call after both WorldGraph and RoadGraph loaded)
    void ComputeBridges();
    const Bridge* GetBridge(uint32_t worldNodeId) const;
    uint32_t GetBridgeCount() const { return static_cast<uint32_t>(m_bridges.size()); }

private:
    RoadGraph() = default;

    // Per-map storage
    struct MapData {
        std::vector<RoadNode>  nodes;
        std::vector<RoadEdge>  edges;
        std::unordered_map<uint32_t, size_t>              nodeIndex;  // id → index
        std::unordered_map<uint32_t, std::vector<size_t>> adjacency;  // nodeId → edge indices
        SpatialGrid grid;
    };

    std::unordered_map<uint32_t, MapData> m_maps;   // mapId → data

    // Bridges: worldNodeId → bridge
    std::unordered_map<uint32_t, Bridge> m_bridges;

    // Find which map a node belongs to (for cross-map A*)
    const MapData* FindMapForNode(uint32_t nodeId) const;

    void BuildAdjacency(MapData& data);

    // Cost ratio and approach distance thresholds
    static constexpr float kMaxCostRatio       = 3.0f;  // road must be <3x direct distance
    static constexpr float kMaxApproachRatio   = 0.5f;  // approach must be <50% of direct
    static constexpr float kBridgeSearchRadius = 300.0f; // bridge search radius in yards
};

} // namespace nav
