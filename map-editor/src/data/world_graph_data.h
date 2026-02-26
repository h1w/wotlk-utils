#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <unordered_map>

namespace mapedit {

enum class NodeType : uint8_t {
    Waypoint, FlightMaster, Portal, BoatZeppelin,
    InnKeeper, ZoneBoundary, DungeonEntrance,
};

enum class EdgeType : uint8_t {
    Walk, Flight, Teleport, Boat,
};

struct WorldNode {
    uint32_t    id    = 0;
    std::string name;
    uint32_t    mapId = 0;
    float       x = 0, y = 0, z = 0;
    NodeType    type  = NodeType::Waypoint;
    std::string faction; // "alliance", "horde", "neutral", or ""
};

struct WorldEdge {
    uint32_t fromNode      = 0;
    uint32_t toNode        = 0;
    EdgeType type          = EdgeType::Walk;
    float    cost          = 0.0f;
    bool     bidirectional = true;
};

const char* NodeTypeToString(NodeType t);
const char* EdgeTypeToString(EdgeType t);
NodeType ParseNodeType(const std::string& s);
EdgeType ParseEdgeType(const std::string& s);

class WorldGraphData {
public:
    bool LoadFromFile(const std::string& path);
    bool SaveToFile(const std::string& path) const;

    // CRUD
    uint32_t AddNode(const WorldNode& node); // Returns assigned ID
    void RemoveNode(uint32_t id);
    WorldNode* GetNode(uint32_t id);
    const WorldNode* GetNode(uint32_t id) const;

    size_t AddEdge(const WorldEdge& edge); // Returns edge index
    void RemoveEdge(size_t index);

    const std::vector<WorldNode>& GetNodes() const { return m_nodes; }
    const std::vector<WorldEdge>& GetEdges() const { return m_edges; }
    std::vector<WorldNode>& GetNodesMut() { return m_nodes; }

    // Replace all graph data (used by undo/redo).
    void SetState(std::vector<WorldNode> nodes, std::vector<WorldEdge> edges);

    bool IsDirty() const { return m_dirty; }
    void ClearDirty() { m_dirty = false; }
    void MarkDirty() { m_dirty = true; ++m_version; }

    uint32_t GetVersion() const { return m_version; }

    bool IsLoaded() const { return m_loaded; }
    const std::string& GetFilePath() const { return m_filePath; }

private:
    std::vector<WorldNode> m_nodes;
    std::vector<WorldEdge> m_edges;
    std::unordered_map<uint32_t, size_t> m_nodeIndex;
    uint32_t m_nextId = 1;
    uint32_t m_version = 0;
    bool m_dirty = false;
    bool m_loaded = false;
    std::string m_filePath;

    void RebuildIndex();
};

} // namespace mapedit
