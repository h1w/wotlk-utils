#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <unordered_map>

namespace mapedit {

class WorldGraphData;

struct GapInfo {
    uint32_t nodeA = 0, nodeB = 0;      // Closest nodes from 2 different components
    int componentA = 0, componentB = 0;  // Component indices
    float distance = 0.0f;
    float midX = 0.0f, midY = 0.0f;     // Midpoint for Go To
};

struct GraphIssue {
    enum Type { Disconnected, DeadEnd, Orphan, DuplicateEdge, ZeroLengthEdge };
    enum Severity { Error, Warning, Info };

    Type type = DeadEnd;
    Severity severity = Warning;
    std::string message;
    float worldX = 0.0f, worldY = 0.0f;
    std::vector<uint32_t> nodeIds;
    std::vector<size_t> edgeIndices;
    std::string key;  // Stable identity for dismiss
    float distance = 0.0f;  // For Disconnected type: gap distance in yards
};

struct ValidationResult {
    std::vector<GraphIssue> issues;
    std::unordered_map<uint32_t, int> nodeComponent;  // nodeId -> componentIndex
    int componentCount = 0;
    std::vector<GapInfo> gaps;
};

class GraphValidator {
public:
    ValidationResult Validate(const WorldGraphData& graph, uint32_t mapId);

    // Count total issues across ALL maps in the graph (lightweight — no GapInfo/components).
    int CountTotalIssues(const WorldGraphData& graph);
};

} // namespace mapedit
