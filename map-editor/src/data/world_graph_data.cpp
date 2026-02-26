#include "world_graph_data.h"

#include <nlohmann/json.hpp>
#include <glog/logging.h>
#include <fstream>
#include <algorithm>

using json = nlohmann::json;

namespace mapedit {

const char* NodeTypeToString(NodeType t) {
    switch (t) {
    case NodeType::FlightMaster:   return "flight_master";
    case NodeType::Portal:         return "portal";
    case NodeType::BoatZeppelin:   return "boat_zeppelin";
    case NodeType::InnKeeper:      return "innkeeper";
    case NodeType::ZoneBoundary:   return "zone_boundary";
    case NodeType::DungeonEntrance:return "dungeon";
    default:                       return "waypoint";
    }
}

const char* EdgeTypeToString(EdgeType t) {
    switch (t) {
    case EdgeType::Flight:  return "flight";
    case EdgeType::Teleport:return "teleport";
    case EdgeType::Boat:    return "boat";
    default:                return "walk";
    }
}

NodeType ParseNodeType(const std::string& s) {
    if (s == "flight_master") return NodeType::FlightMaster;
    if (s == "portal")        return NodeType::Portal;
    if (s == "boat_zeppelin") return NodeType::BoatZeppelin;
    if (s == "innkeeper")     return NodeType::InnKeeper;
    if (s == "zone_boundary") return NodeType::ZoneBoundary;
    if (s == "dungeon")       return NodeType::DungeonEntrance;
    return NodeType::Waypoint;
}

EdgeType ParseEdgeType(const std::string& s) {
    if (s == "flight")  return EdgeType::Flight;
    if (s == "teleport")return EdgeType::Teleport;
    if (s == "boat")    return EdgeType::Boat;
    return EdgeType::Walk;
}

bool WorldGraphData::LoadFromFile(const std::string& path) {
    m_nodes.clear();
    m_edges.clear();
    m_nodeIndex.clear();
    m_nextId = 1;
    m_dirty = false;
    m_loaded = false;

    std::ifstream file(path);
    if (!file.is_open()) {
        LOG(ERROR) << "[WorldGraphData] Cannot open: " << path;
        return false;
    }

    json doc;
    try {
        doc = json::parse(file);
    } catch (const json::parse_error& e) {
        LOG(ERROR) << "[WorldGraphData] Parse error: " << e.what();
        return false;
    }

    if (doc.contains("nodes") && doc["nodes"].is_array()) {
        for (const auto& jn : doc["nodes"]) {
            WorldNode node;
            node.id      = jn.value("id", 0u);
            node.name    = jn.value("name", "");
            node.mapId   = jn.value("map", 0u);
            node.x       = jn.value("x", 0.0f);
            node.y       = jn.value("y", 0.0f);
            node.z       = jn.value("z", 0.0f);
            node.type    = ParseNodeType(jn.value("type", "waypoint"));
            node.faction = jn.value("faction", "");
            m_nodes.push_back(std::move(node));
        }
    }

    // Compute next available ID from all loaded nodes
    for (const auto& n : m_nodes) {
        if (n.id >= m_nextId)
            m_nextId = n.id + 1;
    }

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

    RebuildIndex();
    m_filePath = path;
    m_loaded = true;

    LOG(INFO) << "[WorldGraphData] Loaded " << m_nodes.size() << " nodes, "
              << m_edges.size() << " edges from " << path;
    return true;
}

bool WorldGraphData::SaveToFile(const std::string& path) const {
    json doc;

    json nodesArr = json::array();
    for (const auto& n : m_nodes) {
        json jn;
        jn["id"]   = n.id;
        jn["name"] = n.name;
        jn["map"]  = n.mapId;
        jn["x"]    = n.x;
        jn["y"]    = n.y;
        jn["z"]    = n.z;
        jn["type"] = NodeTypeToString(n.type);
        if (!n.faction.empty())
            jn["faction"] = n.faction;
        nodesArr.push_back(jn);
    }
    doc["nodes"] = nodesArr;

    json edgesArr = json::array();
    for (const auto& e : m_edges) {
        json je;
        je["from"]  = e.fromNode;
        je["to"]    = e.toNode;
        je["type"]  = EdgeTypeToString(e.type);
        je["cost"]  = e.cost;
        je["bidir"] = e.bidirectional;
        edgesArr.push_back(je);
    }
    doc["edges"] = edgesArr;

    std::ofstream file(path);
    if (!file.is_open()) {
        LOG(ERROR) << "[WorldGraphData] Cannot write: " << path;
        return false;
    }

    file << doc.dump(2);
    LOG(INFO) << "[WorldGraphData] Saved to " << path;
    return true;
}

uint32_t WorldGraphData::AddNode(const WorldNode& node) {
    WorldNode n = node;
    n.id = m_nextId++;
    m_nodeIndex[n.id] = m_nodes.size();
    m_nodes.push_back(std::move(n));
    m_dirty = true;
    ++m_version;
    return m_nodes.back().id;
}

void WorldGraphData::RemoveNode(uint32_t id) {
    auto it = m_nodeIndex.find(id);
    if (it == m_nodeIndex.end()) return;

    size_t idx = it->second;
    m_nodes.erase(m_nodes.begin() + idx);

    // Remove edges referencing this node
    m_edges.erase(
        std::remove_if(m_edges.begin(), m_edges.end(),
            [id](const WorldEdge& e) { return e.fromNode == id || e.toNode == id; }),
        m_edges.end());

    RebuildIndex();
    m_dirty = true;
    ++m_version;
}

WorldNode* WorldGraphData::GetNode(uint32_t id) {
    auto it = m_nodeIndex.find(id);
    if (it == m_nodeIndex.end()) return nullptr;
    return &m_nodes[it->second];
}

const WorldNode* WorldGraphData::GetNode(uint32_t id) const {
    auto it = m_nodeIndex.find(id);
    if (it == m_nodeIndex.end()) return nullptr;
    return &m_nodes[it->second];
}

size_t WorldGraphData::AddEdge(const WorldEdge& edge) {
    m_edges.push_back(edge);
    m_dirty = true;
    ++m_version;
    return m_edges.size() - 1;
}

void WorldGraphData::RemoveEdge(size_t index) {
    if (index >= m_edges.size()) return;
    m_edges.erase(m_edges.begin() + index);
    m_dirty = true;
    ++m_version;
}

void WorldGraphData::SetState(std::vector<WorldNode> nodes, std::vector<WorldEdge> edges) {
    m_nodes = std::move(nodes);
    m_edges = std::move(edges);
    m_nextId = 1;
    for (const auto& n : m_nodes) {
        if (n.id >= m_nextId)
            m_nextId = n.id + 1;
    }
    RebuildIndex();
    m_dirty = true;
    ++m_version;
}

void WorldGraphData::RebuildIndex() {
    m_nodeIndex.clear();
    for (size_t i = 0; i < m_nodes.size(); ++i)
        m_nodeIndex[m_nodes[i].id] = i;
}

} // namespace mapedit
