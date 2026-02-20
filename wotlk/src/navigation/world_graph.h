#pragma once
// =============================================================================
// WorldGraph — strategic-level route planning across zones and continents.
//
// Loads a JSON graph of world nodes (waypoints, flight masters, portals, etc.)
// and edges (walk, flight, teleport, boat). A* search finds the optimal route
// between any two nodes. Tier 2 (Detour navmesh) handles the actual Walk
// segments, while other edge types require game interaction.
// =============================================================================

#include "../game/types.h"

#include <cstdint>
#include <string>
#include <vector>
#include <unordered_map>

namespace nav {

// ---- World Node (graph vertex) ----

enum class NodeType : uint8_t {
    Waypoint,
    FlightMaster,
    Portal,
    BoatZeppelin,
    InnKeeper,
    ZoneBoundary,
    DungeonEntrance,
};

struct WorldNode {
    uint32_t    id       = 0;
    std::string name;
    uint32_t    mapId    = 0;     // 0=EK, 1=Kalimdor, 530=Outland, 571=Northrend
    game::Vec3  pos{};
    NodeType    type     = NodeType::Waypoint;
};

// ---- World Edge (graph edge) ----

enum class EdgeType : uint8_t {
    Walk,       // Navigate via navmesh
    Flight,     // Take flight master taxi
    Teleport,   // Portal / loading screen
    Boat,       // Boat/zeppelin (wait for transport)
};

struct WorldEdge {
    uint32_t fromNode      = 0;
    uint32_t toNode        = 0;
    EdgeType type          = EdgeType::Walk;
    float    cost          = 0.0f;   // estimated travel time in seconds
    bool     bidirectional = true;
};

// ---- Route segment (result of A* planning) ----

struct RouteSegment {
    uint32_t fromNodeId;
    uint32_t toNodeId;
    EdgeType edgeType;
    game::Vec3 fromPos;
    game::Vec3 toPos;
    float    cost;
};

// ---- WorldGraph ----

class WorldGraph {
public:
    static WorldGraph& Instance();

    // Load graph from JSON file. Returns false on parse error.
    bool LoadFromFile(const char* jsonPath);

    // Unload all data.
    void Clear();

    bool IsLoaded() const { return !m_nodes.empty(); }
    int  GetNodeCount() const { return static_cast<int>(m_nodes.size()); }
    int  GetEdgeCount() const { return static_cast<int>(m_edges.size()); }

    // Find nearest node on a given map within maxDist (2D distance in yards).
    const WorldNode* FindNearestNode(uint32_t mapId, float x, float y,
                                     float maxDist = 2000.0f) const;

    // Find node by ID.
    const WorldNode* GetNode(uint32_t id) const;

    // Find node by name (case-insensitive substring match).
    const WorldNode* FindNodeByName(const std::string& name) const;

    // Plan route from startNode to endNode via A*.
    // Returns empty vector if no path found.
    std::vector<RouteSegment> PlanRoute(uint32_t startNodeId, uint32_t endNodeId) const;

    // Convenience: plan route from world position to a target node.
    // Finds nearest node to player position, then plans route.
    std::vector<RouteSegment> PlanRouteFromPos(uint32_t mapId, float x, float y,
                                               uint32_t endNodeId) const;

    // Access raw data (for debug/UI).
    const std::vector<WorldNode>& GetNodes() const { return m_nodes; }
    const std::vector<WorldEdge>& GetEdges() const { return m_edges; }

private:
    WorldGraph() = default;

    std::vector<WorldNode> m_nodes;
    std::vector<WorldEdge> m_edges;

    // Node ID -> index in m_nodes (for fast lookup)
    std::unordered_map<uint32_t, size_t> m_nodeIndex;

    // Adjacency list: nodeId -> list of edge indices in m_edges
    std::unordered_map<uint32_t, std::vector<size_t>> m_adjacency;

    void BuildAdjacency();

    // A* heuristic: straight-line distance / assumed speed
    static float Heuristic(const WorldNode& a, const WorldNode& b);
};

} // namespace nav
