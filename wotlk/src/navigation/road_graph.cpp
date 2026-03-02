#include "road_graph.h"
#include "world_graph.h"

#include <nlohmann/json.hpp>
#include <glog/logging.h>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <queue>
#include <unordered_set>

using json = nlohmann::json;

namespace nav {

// ---------------------------------------------------------------------------
// SpatialGrid
// ---------------------------------------------------------------------------

void SpatialGrid::Clear() {
    cells.clear();
}

void SpatialGrid::Insert(uint32_t nodeId, float x, float y) {
    CellKey key;
    key.cx = static_cast<int32_t>(std::floor(x / kCellSize));
    key.cy = static_cast<int32_t>(std::floor(y / kCellSize));
    cells[key].push_back(nodeId);
}

uint32_t SpatialGrid::FindNearest(float x, float y, float z, float maxRadius,
                                   const std::vector<RoadNode>& nodes,
                                   const std::unordered_map<uint32_t, size_t>& nodeIndex) const {
    int32_t centerCx = static_cast<int32_t>(std::floor(x / kCellSize));
    int32_t centerCy = static_cast<int32_t>(std::floor(y / kCellSize));

    // Determine how many cells to search based on radius
    int32_t cellRange = static_cast<int32_t>(std::ceil(maxRadius / kCellSize));

    uint32_t bestId = 0;
    float bestDist2 = maxRadius * maxRadius;

    for (int32_t dx = -cellRange; dx <= cellRange; ++dx) {
        for (int32_t dy = -cellRange; dy <= cellRange; ++dy) {
            CellKey key;
            key.cx = centerCx + dx;
            key.cy = centerCy + dy;

            auto it = cells.find(key);
            if (it == cells.end())
                continue;

            for (uint32_t nodeId : it->second) {
                auto indexIt = nodeIndex.find(nodeId);
                if (indexIt == nodeIndex.end()) continue;

                const RoadNode& node = nodes[indexIt->second];
                float ndx = node.x - x;
                float ndy = node.y - y;
                float ndz = node.z - z;
                float d2 = ndx * ndx + ndy * ndy + ndz * ndz;

                if (d2 < bestDist2) {
                    bestDist2 = d2;
                    bestId = nodeId;
                }
            }
        }
    }

    return bestId;
}

// ---------------------------------------------------------------------------
// RoadGraph — Singleton
// ---------------------------------------------------------------------------

RoadGraph& RoadGraph::Instance() {
    static RoadGraph s_instance;
    return s_instance;
}

// ---------------------------------------------------------------------------
// JSON Loading
// ---------------------------------------------------------------------------

bool RoadGraph::LoadFromFile(uint32_t mapId, const char* jsonPath) {
    // Remove previous data for this mapId and invalidate bridges
    m_maps.erase(mapId);
    m_bridges.clear();

    std::ifstream file(jsonPath);
    if (!file.is_open()) {
        LOG(ERROR) << "[RoadGraph] Cannot open: " << jsonPath;
        return false;
    }

    json doc;
    try {
        doc = json::parse(file);
    } catch (const json::parse_error& e) {
        LOG(ERROR) << "[RoadGraph] JSON parse error: " << e.what();
        return false;
    }

    MapData data;

    // Parse nodes (ID 0 is reserved as "not found" sentinel — skip it)
    if (doc.contains("nodes") && doc["nodes"].is_array()) {
        for (const auto& jn : doc["nodes"]) {
            RoadNode node;
            node.id    = jn.value("id", 0u);
            if (node.id == 0) {
                LOG(WARNING) << "[RoadGraph] Skipping node with id=0 (reserved as sentinel)";
                continue;
            }
            node.x     = jn.value("x", 0.0f);
            node.y     = jn.value("y", 0.0f);
            node.z     = jn.value("z", 0.0f);
            node.mapId = jn.value("map", mapId);
            data.nodes.push_back(node);
        }
    }

    // Parse edges
    if (doc.contains("edges") && doc["edges"].is_array()) {
        for (const auto& je : doc["edges"]) {
            RoadEdge edge;
            edge.from = je.value("from", 0u);
            edge.to   = je.value("to", 0u);
            edge.cost = je.value("cost", 0.0f);
            data.edges.push_back(edge);
        }
    }

    // Build node index
    for (size_t i = 0; i < data.nodes.size(); ++i)
        data.nodeIndex[data.nodes[i].id] = i;

    // Build adjacency list
    BuildAdjacency(data);

    // Build spatial grid
    for (const auto& node : data.nodes)
        data.grid.Insert(node.id, node.x, node.y);

    LOG(INFO) << "[RoadGraph] Loaded map " << mapId << ": "
              << data.nodes.size() << " nodes, "
              << data.edges.size() << " edges from " << jsonPath;

    m_maps[mapId] = std::move(data);
    return true;
}

void RoadGraph::Clear() {
    m_maps.clear();
    m_bridges.clear();
}

bool RoadGraph::IsLoaded(uint32_t mapId) const {
    return m_maps.count(mapId) > 0;
}

void RoadGraph::BuildAdjacency(MapData& data) {
    data.adjacency.clear();
    for (size_t i = 0; i < data.edges.size(); ++i) {
        // All road edges are bidirectional
        data.adjacency[data.edges[i].from].push_back(i);
        data.adjacency[data.edges[i].to].push_back(i);
    }
}

// ---------------------------------------------------------------------------
// Queries
// ---------------------------------------------------------------------------

const RoadNode* RoadGraph::GetNode(uint32_t id) const {
    for (const auto& [mapId, data] : m_maps) {
        auto it = data.nodeIndex.find(id);
        if (it != data.nodeIndex.end())
            return &data.nodes[it->second];
    }
    return nullptr;
}

const RoadGraph::MapData* RoadGraph::FindMapForNode(uint32_t nodeId) const {
    for (const auto& [mapId, data] : m_maps) {
        if (data.nodeIndex.count(nodeId))
            return &data;
    }
    return nullptr;
}

const RoadNode* RoadGraph::FindNearestNode(uint32_t mapId, float x, float y, float z,
                                            float maxRadius) const {
    auto it = m_maps.find(mapId);
    if (it == m_maps.end())
        return nullptr;

    const MapData& data = it->second;
    uint32_t bestId = data.grid.FindNearest(x, y, z, maxRadius, data.nodes, data.nodeIndex);
    if (bestId == 0)
        return nullptr;

    auto nodeIt = data.nodeIndex.find(bestId);
    if (nodeIt == data.nodeIndex.end())
        return nullptr;

    return &data.nodes[nodeIt->second];
}

uint32_t RoadGraph::GetNodeCount(uint32_t mapId) const {
    auto it = m_maps.find(mapId);
    return it != m_maps.end() ? static_cast<uint32_t>(it->second.nodes.size()) : 0;
}

uint32_t RoadGraph::GetEdgeCount(uint32_t mapId) const {
    auto it = m_maps.find(mapId);
    return it != m_maps.end() ? static_cast<uint32_t>(it->second.edges.size()) : 0;
}

// ---------------------------------------------------------------------------
// A* Pathfinding
// ---------------------------------------------------------------------------

std::vector<uint32_t> RoadGraph::FindPath(uint32_t fromId, uint32_t toId) const {
    if (fromId == toId)
        return { fromId };

    const MapData* mapData = FindMapForNode(fromId);
    if (!mapData)
        return {};

    // Verify both nodes are in the same map data
    if (!mapData->nodeIndex.count(toId))
        return {};

    const auto& nodes = mapData->nodes;
    const auto& edges = mapData->edges;
    const auto& nodeIndex = mapData->nodeIndex;
    const auto& adjacency = mapData->adjacency;

    auto toIt = nodeIndex.find(toId);
    if (toIt == nodeIndex.end())
        return {};
    const RoadNode& endNode = nodes[toIt->second];

    // A* open set: (f-cost, nodeId)
    using PQEntry = std::pair<float, uint32_t>;
    std::priority_queue<PQEntry, std::vector<PQEntry>, std::greater<>> openSet;

    std::unordered_map<uint32_t, float>    gScore;
    std::unordered_map<uint32_t, uint32_t> cameFrom;
    std::unordered_set<uint32_t>           closed;

    // Heuristic: 3D Euclidean distance (admissible for road graph where cost = distance)
    auto heuristic = [&](uint32_t nodeId) -> float {
        auto it = nodeIndex.find(nodeId);
        if (it == nodeIndex.end()) return 0.f;
        const RoadNode& n = nodes[it->second];
        float dx = n.x - endNode.x;
        float dy = n.y - endNode.y;
        float dz = n.z - endNode.z;
        return std::sqrt(dx * dx + dy * dy + dz * dz);
    };

    gScore[fromId] = 0.0f;
    openSet.push({ heuristic(fromId), fromId });

    while (!openSet.empty()) {
        auto [fCost, currentId] = openSet.top();
        openSet.pop();

        if (currentId == toId) {
            // Reconstruct path
            std::vector<uint32_t> path;
            uint32_t nodeId = toId;
            while (true) {
                path.push_back(nodeId);
                auto it = cameFrom.find(nodeId);
                if (it == cameFrom.end()) break;
                nodeId = it->second;
            }
            std::reverse(path.begin(), path.end());
            return path;
        }

        if (closed.count(currentId))
            continue;
        closed.insert(currentId);

        auto adjIt = adjacency.find(currentId);
        if (adjIt == adjacency.end())
            continue;

        float currentG = gScore[currentId];

        for (size_t edgeIdx : adjIt->second) {
            const RoadEdge& edge = edges[edgeIdx];

            // All edges are bidirectional — determine neighbor
            uint32_t neighborId;
            if (edge.from == currentId)
                neighborId = edge.to;
            else
                neighborId = edge.from;

            if (closed.count(neighborId))
                continue;

            float tentativeG = currentG + edge.cost;
            auto gIt = gScore.find(neighborId);
            if (gIt != gScore.end() && tentativeG >= gIt->second)
                continue;

            gScore[neighborId]   = tentativeG;
            cameFrom[neighborId] = currentId;

            float h = heuristic(neighborId);
            openSet.push({ tentativeG + h, neighborId });
        }
    }

    return {}; // no path
}

// ---------------------------------------------------------------------------
// Road Path Planning
// ---------------------------------------------------------------------------

RoadPlan RoadGraph::PlanRoadPath(const game::Vec3& start, const game::Vec3& end,
                                  uint32_t mapId) const {
    RoadPlan plan{};
    plan.directDist = start.DistanceTo(end);

    // 1. Find nearest road nodes
    const auto* entry = FindNearestNode(mapId, start.x, start.y, start.z, 200.0f);
    const auto* exit  = FindNearestNode(mapId, end.x, end.y, end.z, 200.0f);

    if (!entry || !exit) {
        plan.useRoad = false;
        return plan;
    }

    if (entry->id == exit->id) {
        plan.useRoad = false;
        return plan;
    }

    // 2. A* on road graph
    auto roadPath = FindPath(entry->id, exit->id);
    if (roadPath.empty()) {
        plan.useRoad = false;
        return plan;
    }

    // 3. Compute costs
    plan.entryNodeId = entry->id;
    plan.exitNodeId  = exit->id;
    plan.roadPath    = std::move(roadPath);

    game::Vec3 entryPos{ entry->x, entry->y, entry->z };
    game::Vec3 exitPos{ exit->x, exit->y, exit->z };

    plan.approachDist  = start.DistanceTo(entryPos);
    plan.departureDist = exitPos.DistanceTo(end);

    // Sum road edge costs (recompute from node positions for accuracy)
    plan.roadDist = 0.f;
    for (size_t i = 0; i + 1 < plan.roadPath.size(); ++i) {
        const auto* a = GetNode(plan.roadPath[i]);
        const auto* b = GetNode(plan.roadPath[i + 1]);
        if (a && b) {
            game::Vec3 pa{ a->x, a->y, a->z };
            game::Vec3 pb{ b->x, b->y, b->z };
            plan.roadDist += pa.DistanceTo(pb);
        }
    }

    plan.totalDist = plan.approachDist + plan.roadDist + plan.departureDist;

    // 4. Approach distance check: >50% of direct → don't use road
    if (plan.approachDist > plan.directDist * kMaxApproachRatio) {
        plan.useRoad = false;
        LOG(INFO) << "[RoadNav] Approach too far (" << plan.approachDist
                  << " yd vs " << plan.directDist << " yd direct)";
        return plan;
    }

    // 5. Cost ratio check: >3x direct → don't use road
    if (plan.totalDist > plan.directDist * kMaxCostRatio) {
        plan.useRoad = false;
        LOG(INFO) << "[RoadNav] Road path too long: " << plan.totalDist
                  << " vs direct " << plan.directDist
                  << " (ratio " << plan.totalDist / plan.directDist << ")";
        return plan;
    }

    plan.useRoad = true;
    LOG(INFO) << "[RoadNav] Road path: " << plan.roadPath.size() << " nodes, "
              << plan.roadDist << " yd road + "
              << plan.approachDist << " yd approach + "
              << plan.departureDist << " yd departure";
    return plan;
}

// ---------------------------------------------------------------------------
// Bridge Computation
// ---------------------------------------------------------------------------

void RoadGraph::ComputeBridges() {
    m_bridges.clear();

    auto& wg = WorldGraph::Instance();
    if (!wg.IsLoaded())
        return;

    for (const auto& wnode : wg.GetNodes()) {
        // Only bridge walkable node types
        switch (wnode.type) {
        case NodeType::FlightMaster:
        case NodeType::Portal:
        case NodeType::BoatZeppelin:
        case NodeType::InnKeeper:
        case NodeType::ZoneBoundary:
        case NodeType::DungeonEntrance:
        case NodeType::Waypoint:
            break;
        default:
            continue;
        }

        const auto* rnode = FindNearestNode(wnode.mapId,
                                             wnode.pos.x, wnode.pos.y, wnode.pos.z,
                                             kBridgeSearchRadius);
        if (rnode) {
            game::Vec3 rnodePos{ rnode->x, rnode->y, rnode->z };
            Bridge bridge;
            bridge.worldNodeId = wnode.id;
            bridge.roadNodeId  = rnode->id;
            bridge.distance    = wnode.pos.DistanceTo(rnodePos);
            m_bridges[wnode.id] = bridge;
        }
    }

    LOG(INFO) << "[RoadGraph] Computed " << m_bridges.size() << " bridges to WorldGraph";
}

const Bridge* RoadGraph::GetBridge(uint32_t worldNodeId) const {
    auto it = m_bridges.find(worldNodeId);
    return it != m_bridges.end() ? &it->second : nullptr;
}

} // namespace nav
