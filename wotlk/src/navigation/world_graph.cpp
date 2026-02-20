#include "world_graph.h"

#include <nlohmann/json.hpp>
#include <glog/logging.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <queue>
#include <unordered_set>

using json = nlohmann::json;

namespace nav {

// Assumed average movement speed in yards/sec for heuristic.
// Run speed ~7 yd/s, mount ~14 yd/s — use a generous estimate so A* doesn't
// over-expand nodes (heuristic must be admissible, i.e. <= actual cost).
static constexpr float kAssumedSpeed = 14.0f;

// Cross-map heuristic penalty: if nodes are on different maps, add a fixed
// cost to represent the loading screen / transport time.
static constexpr float kCrossMapPenalty = 60.0f;

// ---------------------------------------------------------------------------
// Singleton
// ---------------------------------------------------------------------------

WorldGraph& WorldGraph::Instance() {
    static WorldGraph s_instance;
    return s_instance;
}

// ---------------------------------------------------------------------------
// JSON loading
// ---------------------------------------------------------------------------

static NodeType ParseNodeType(const std::string& s) {
    if (s == "flight_master") return NodeType::FlightMaster;
    if (s == "portal")        return NodeType::Portal;
    if (s == "boat_zeppelin") return NodeType::BoatZeppelin;
    if (s == "innkeeper")     return NodeType::InnKeeper;
    if (s == "zone_boundary") return NodeType::ZoneBoundary;
    if (s == "dungeon")       return NodeType::DungeonEntrance;
    return NodeType::Waypoint;
}

static EdgeType ParseEdgeType(const std::string& s) {
    if (s == "flight")  return EdgeType::Flight;
    if (s == "teleport") return EdgeType::Teleport;
    if (s == "boat")    return EdgeType::Boat;
    return EdgeType::Walk;
}

bool WorldGraph::LoadFromFile(const char* jsonPath) {
    Clear();

    std::ifstream file(jsonPath);
    if (!file.is_open()) {
        LOG(ERROR) << "[WorldGraph] Cannot open: " << jsonPath;
        return false;
    }

    json doc;
    try {
        doc = json::parse(file);
    } catch (const json::parse_error& e) {
        LOG(ERROR) << "[WorldGraph] JSON parse error: " << e.what();
        return false;
    }

    // Parse nodes
    if (doc.contains("nodes") && doc["nodes"].is_array()) {
        for (const auto& jn : doc["nodes"]) {
            WorldNode node;
            node.id    = jn.value("id", 0u);
            node.name  = jn.value("name", "");
            node.mapId = jn.value("map", 0u);
            node.pos.x = jn.value("x", 0.0f);
            node.pos.y = jn.value("y", 0.0f);
            node.pos.z = jn.value("z", 0.0f);
            node.type  = ParseNodeType(jn.value("type", "waypoint"));
            m_nodes.push_back(std::move(node));
        }
    }

    // Parse edges
    if (doc.contains("edges") && doc["edges"].is_array()) {
        for (const auto& je : doc["edges"]) {
            WorldEdge edge;
            edge.fromNode      = je.value("from", 0u);
            edge.toNode        = je.value("to", 0u);
            edge.type          = ParseEdgeType(je.value("type", "walk"));
            edge.cost          = je.value("cost", 0.0f);
            edge.bidirectional = je.value("bidir", true);
            m_edges.push_back(edge);
        }
    }

    // Build index and adjacency
    for (size_t i = 0; i < m_nodes.size(); ++i)
        m_nodeIndex[m_nodes[i].id] = i;

    BuildAdjacency();

    LOG(INFO) << "[WorldGraph] Loaded " << m_nodes.size() << " nodes, "
              << m_edges.size() << " edges from " << jsonPath;
    return true;
}

void WorldGraph::Clear() {
    m_nodes.clear();
    m_edges.clear();
    m_nodeIndex.clear();
    m_adjacency.clear();
}

void WorldGraph::BuildAdjacency() {
    m_adjacency.clear();
    for (size_t i = 0; i < m_edges.size(); ++i) {
        m_adjacency[m_edges[i].fromNode].push_back(i);
        if (m_edges[i].bidirectional)
            m_adjacency[m_edges[i].toNode].push_back(i);
    }
}

// ---------------------------------------------------------------------------
// Lookups
// ---------------------------------------------------------------------------

const WorldNode* WorldGraph::GetNode(uint32_t id) const {
    auto it = m_nodeIndex.find(id);
    if (it == m_nodeIndex.end()) return nullptr;
    return &m_nodes[it->second];
}

const WorldNode* WorldGraph::FindNearestNode(uint32_t mapId, float x, float y,
                                              float maxDist) const {
    const WorldNode* best = nullptr;
    float bestDist = maxDist * maxDist; // compare squared

    for (const auto& n : m_nodes) {
        if (n.mapId != mapId) continue;
        float dx = n.pos.x - x;
        float dy = n.pos.y - y;
        float d2 = dx * dx + dy * dy;
        if (d2 < bestDist) {
            bestDist = d2;
            best = &n;
        }
    }

    return best;
}

const WorldNode* WorldGraph::FindNodeByName(const std::string& name) const {
    // Case-insensitive substring match
    std::string lower;
    lower.resize(name.size());
    std::transform(name.begin(), name.end(), lower.begin(),
                   [](char c) { return static_cast<char>(std::tolower(c)); });

    for (const auto& n : m_nodes) {
        std::string nodeLower;
        nodeLower.resize(n.name.size());
        std::transform(n.name.begin(), n.name.end(), nodeLower.begin(),
                       [](char c) { return static_cast<char>(std::tolower(c)); });
        if (nodeLower.find(lower) != std::string::npos)
            return &n;
    }

    return nullptr;
}

// ---------------------------------------------------------------------------
// A* Route Planning
// ---------------------------------------------------------------------------

float WorldGraph::Heuristic(const WorldNode& a, const WorldNode& b) {
    if (a.mapId != b.mapId)
        return kCrossMapPenalty;

    float dx = a.pos.x - b.pos.x;
    float dy = a.pos.y - b.pos.y;
    float dist = std::sqrt(dx * dx + dy * dy);
    return dist / kAssumedSpeed;
}

std::vector<RouteSegment> WorldGraph::PlanRoute(uint32_t startId, uint32_t endId) const {
    if (startId == endId) return {};

    auto startIt = m_nodeIndex.find(startId);
    auto endIt   = m_nodeIndex.find(endId);
    if (startIt == m_nodeIndex.end() || endIt == m_nodeIndex.end())
        return {};

    const WorldNode& endNode = m_nodes[endIt->second];

    // A* open set: (f-cost, nodeId)
    using PQEntry = std::pair<float, uint32_t>;
    std::priority_queue<PQEntry, std::vector<PQEntry>, std::greater<>> openSet;

    std::unordered_map<uint32_t, float>    gScore;    // cost from start
    std::unordered_map<uint32_t, uint32_t> cameFrom;  // node -> predecessor
    std::unordered_map<uint32_t, size_t>   cameEdge;  // node -> edge index used
    std::unordered_set<uint32_t>           closed;

    gScore[startId] = 0.0f;
    openSet.push({Heuristic(m_nodes[startIt->second], endNode), startId});

    while (!openSet.empty()) {
        auto [fCost, currentId] = openSet.top();
        openSet.pop();

        if (currentId == endId) {
            // Reconstruct path
            std::vector<RouteSegment> route;
            uint32_t nodeId = endId;
            while (cameFrom.count(nodeId)) {
                uint32_t prevId = cameFrom[nodeId];
                size_t edgeIdx = cameEdge[nodeId];
                const WorldEdge& edge = m_edges[edgeIdx];
                const WorldNode* from = GetNode(prevId);
                const WorldNode* to   = GetNode(nodeId);

                RouteSegment seg;
                seg.fromNodeId = prevId;
                seg.toNodeId   = nodeId;
                seg.edgeType   = edge.type;
                seg.fromPos    = from ? from->pos : game::Vec3{};
                seg.toPos      = to   ? to->pos   : game::Vec3{};
                seg.cost       = edge.cost;
                route.push_back(seg);

                nodeId = prevId;
            }
            std::reverse(route.begin(), route.end());
            return route;
        }

        if (closed.count(currentId))
            continue;
        closed.insert(currentId);

        auto adjIt = m_adjacency.find(currentId);
        if (adjIt == m_adjacency.end())
            continue;

        float currentG = gScore[currentId];

        for (size_t edgeIdx : adjIt->second) {
            const WorldEdge& edge = m_edges[edgeIdx];

            // Determine neighbor: for bidirectional edges, neighbor is the other end
            uint32_t neighborId;
            if (edge.fromNode == currentId)
                neighborId = edge.toNode;
            else if (edge.bidirectional && edge.toNode == currentId)
                neighborId = edge.fromNode;
            else
                continue;

            if (closed.count(neighborId))
                continue;

            float tentativeG = currentG + edge.cost;
            auto gIt = gScore.find(neighborId);
            if (gIt != gScore.end() && tentativeG >= gIt->second)
                continue;

            gScore[neighborId]   = tentativeG;
            cameFrom[neighborId] = currentId;
            cameEdge[neighborId] = edgeIdx;

            auto neighborNodeIt = m_nodeIndex.find(neighborId);
            if (neighborNodeIt == m_nodeIndex.end()) continue;
            float h = Heuristic(m_nodes[neighborNodeIt->second], endNode);
            openSet.push({tentativeG + h, neighborId});
        }
    }

    LOG(WARNING) << "[WorldGraph] No route from node " << startId << " to " << endId;
    return {};
}

std::vector<RouteSegment> WorldGraph::PlanRouteFromPos(uint32_t mapId, float x, float y,
                                                        uint32_t endNodeId) const {
    const WorldNode* nearest = FindNearestNode(mapId, x, y);
    if (!nearest) {
        LOG(WARNING) << "[WorldGraph] No nearby node for pos (" << x << ", " << y
                     << ") on map " << mapId;
        return {};
    }
    return PlanRoute(nearest->id, endNodeId);
}

} // namespace nav
