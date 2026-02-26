#include "graph_validator.h"
#include "../data/world_graph_data.h"

#include <algorithm>
#include <cmath>
#include <unordered_set>
#include <cstdio>

namespace mapedit {

ValidationResult GraphValidator::Validate(const WorldGraphData& graph, uint32_t mapId) {
    ValidationResult result;

    // Build degree map for nodes on this map
    std::unordered_map<uint32_t, int> degree;
    for (const auto& node : graph.GetNodes()) {
        if (node.mapId == mapId)
            degree[node.id] = 0;
    }
    if (degree.empty())
        return result;

    for (const auto& edge : graph.GetEdges()) {
        if (degree.count(edge.fromNode)) degree[edge.fromNode]++;
        if (degree.count(edge.toNode))   degree[edge.toNode]++;
    }

    // Dead-ends (degree==1) and orphans (degree==0)
    for (auto& [id, deg] : degree) {
        if (deg == 0) {
            const auto* node = graph.GetNode(id);
            GraphIssue issue;
            issue.type = GraphIssue::Orphan;
            issue.severity = GraphIssue::Warning;
            issue.nodeIds.push_back(id);
            if (node) {
                issue.worldX = node->x;
                issue.worldY = node->y;
            }
            char buf[128];
            snprintf(buf, sizeof(buf), "Orphan node #%u (no edges)", id);
            issue.message = buf;
            snprintf(buf, sizeof(buf), "orphan:%u", id);
            issue.key = buf;
            result.issues.push_back(std::move(issue));
        } else if (deg == 1) {
            const auto* node = graph.GetNode(id);
            GraphIssue issue;
            issue.type = GraphIssue::DeadEnd;
            issue.severity = GraphIssue::Info;
            issue.nodeIds.push_back(id);
            if (node) {
                issue.worldX = node->x;
                issue.worldY = node->y;
            }
            char buf[128];
            snprintf(buf, sizeof(buf), "Dead-end node #%u (degree 1)", id);
            issue.message = buf;
            snprintf(buf, sizeof(buf), "dead:%u", id);
            issue.key = buf;
            result.issues.push_back(std::move(issue));
        }
    }

    // BFS to assign connected components
    std::unordered_map<uint32_t, std::vector<uint32_t>> adj;
    for (const auto& edge : graph.GetEdges()) {
        if (degree.count(edge.fromNode) && degree.count(edge.toNode)) {
            adj[edge.fromNode].push_back(edge.toNode);
            adj[edge.toNode].push_back(edge.fromNode);
        }
    }

    int compIdx = 0;
    std::unordered_set<uint32_t> visited;
    std::vector<std::vector<uint32_t>> components; // nodes per component

    for (auto& [id, deg] : degree) {
        if (visited.count(id)) continue;
        std::vector<uint32_t> comp;
        std::vector<uint32_t> queue = {id};
        visited.insert(id);
        while (!queue.empty()) {
            uint32_t cur = queue.back(); queue.pop_back();
            comp.push_back(cur);
            result.nodeComponent[cur] = compIdx;
            for (uint32_t nb : adj[cur]) {
                if (!visited.count(nb)) {
                    visited.insert(nb);
                    queue.push_back(nb);
                }
            }
        }
        components.push_back(std::move(comp));
        ++compIdx;
    }
    result.componentCount = compIdx;

    // Sort components by size (largest first) and reassign indices
    if (result.componentCount > 1) {
        std::vector<int> order(result.componentCount);
        for (int i = 0; i < result.componentCount; ++i) order[i] = i;
        std::sort(order.begin(), order.end(), [&](int a, int b) {
            return components[a].size() > components[b].size();
        });

        // Build remapping: old index -> new index
        std::vector<int> remap(result.componentCount);
        for (int newIdx = 0; newIdx < result.componentCount; ++newIdx)
            remap[order[newIdx]] = newIdx;

        // Reorder components vector
        std::vector<std::vector<uint32_t>> reordered(result.componentCount);
        for (int i = 0; i < result.componentCount; ++i)
            reordered[remap[i]] = std::move(components[i]);
        components = std::move(reordered);

        // Update nodeComponent map
        for (auto& [nodeId, ci] : result.nodeComponent)
            ci = remap[ci];
    }

    // Find gaps between component pairs (closest node pairs)
    if (result.componentCount > 1 && result.componentCount <= 50) {
        for (int a = 0; a < result.componentCount; ++a) {
            for (int b = a + 1; b < result.componentCount; ++b) {
                float bestDist = 1e18f;
                uint32_t bestA = 0, bestB = 0;
                for (uint32_t na : components[a]) {
                    const auto* nodeA = graph.GetNode(na);
                    if (!nodeA) continue;
                    for (uint32_t nb : components[b]) {
                        const auto* nodeB = graph.GetNode(nb);
                        if (!nodeB) continue;
                        float dx = nodeA->x - nodeB->x;
                        float dy = nodeA->y - nodeB->y;
                        float d = dx * dx + dy * dy;
                        if (d < bestDist) {
                            bestDist = d;
                            bestA = na;
                            bestB = nb;
                        }
                    }
                }
                if (bestA && bestB) {
                    bestDist = std::sqrt(bestDist);
                    const auto* nA = graph.GetNode(bestA);
                    const auto* nB = graph.GetNode(bestB);
                    GapInfo gap;
                    gap.nodeA = bestA;
                    gap.nodeB = bestB;
                    gap.componentA = a;
                    gap.componentB = b;
                    gap.distance = bestDist;
                    gap.midX = (nA->x + nB->x) * 0.5f;
                    gap.midY = (nA->y + nB->y) * 0.5f;
                    result.gaps.push_back(gap);

                    // Create a Disconnected issue for this gap
                    uint32_t minId = (std::min)(bestA, bestB);
                    uint32_t maxId = (std::max)(bestA, bestB);
                    GraphIssue issue;
                    issue.type = GraphIssue::Disconnected;
                    issue.severity = GraphIssue::Error;
                    issue.worldX = gap.midX;
                    issue.worldY = gap.midY;
                    issue.nodeIds = {bestA, bestB};
                    issue.distance = bestDist;
                    char buf[256];
                    snprintf(buf, sizeof(buf),
                             "Gap between components %d and %d (%.0f yards, nodes #%u-#%u)",
                             a, b, bestDist, bestA, bestB);
                    issue.message = buf;
                    snprintf(buf, sizeof(buf), "gap:%u:%u", minId, maxId);
                    issue.key = buf;
                    result.issues.push_back(std::move(issue));
                }
            }
        }
    }

    // Duplicate edges
    for (size_t i = 0; i < graph.GetEdges().size(); ++i) {
        const auto& ei = graph.GetEdges()[i];
        if (!degree.count(ei.fromNode) || !degree.count(ei.toNode)) continue;
        for (size_t j = 0; j < i; ++j) {
            const auto& ej = graph.GetEdges()[j];
            if ((ei.fromNode == ej.fromNode && ei.toNode == ej.toNode) ||
                (ei.fromNode == ej.toNode && ei.toNode == ej.fromNode)) {
                uint32_t minId = (std::min)(ei.fromNode, ei.toNode);
                uint32_t maxId = (std::max)(ei.fromNode, ei.toNode);
                const auto* from = graph.GetNode(ei.fromNode);
                const auto* to = graph.GetNode(ei.toNode);
                GraphIssue issue;
                issue.type = GraphIssue::DuplicateEdge;
                issue.severity = GraphIssue::Warning;
                issue.edgeIndices = {j, i};
                issue.nodeIds = {ei.fromNode, ei.toNode};
                if (from && to) {
                    issue.worldX = (from->x + to->x) * 0.5f;
                    issue.worldY = (from->y + to->y) * 0.5f;
                }
                char buf[128];
                snprintf(buf, sizeof(buf), "Duplicate edge #%u-#%u", minId, maxId);
                issue.message = buf;
                snprintf(buf, sizeof(buf), "dup:%u:%u", minId, maxId);
                issue.key = buf;
                result.issues.push_back(std::move(issue));
                break;
            }
        }
    }

    // Zero-length edges
    for (size_t i = 0; i < graph.GetEdges().size(); ++i) {
        const auto& edge = graph.GetEdges()[i];
        if (!degree.count(edge.fromNode) || !degree.count(edge.toNode)) continue;
        const auto* from = graph.GetNode(edge.fromNode);
        const auto* to = graph.GetNode(edge.toNode);
        if (!from || !to) continue;
        float dx = from->x - to->x;
        float dy = from->y - to->y;
        if (std::sqrt(dx * dx + dy * dy) < 0.1f) {
            uint32_t minId = (std::min)(edge.fromNode, edge.toNode);
            uint32_t maxId = (std::max)(edge.fromNode, edge.toNode);
            GraphIssue issue;
            issue.type = GraphIssue::ZeroLengthEdge;
            issue.severity = GraphIssue::Warning;
            issue.edgeIndices = {i};
            issue.nodeIds = {edge.fromNode, edge.toNode};
            issue.worldX = from->x;
            issue.worldY = from->y;
            char buf[128];
            snprintf(buf, sizeof(buf), "Zero-length edge #%u-#%u", minId, maxId);
            issue.message = buf;
            snprintf(buf, sizeof(buf), "zero:%u:%u", minId, maxId);
            issue.key = buf;
            result.issues.push_back(std::move(issue));
        }
    }

    // Sort issues: errors first, then warnings, then info
    std::stable_sort(result.issues.begin(), result.issues.end(),
        [](const GraphIssue& a, const GraphIssue& b) {
            return a.severity < b.severity;
        });

    return result;
}

int GraphValidator::CountTotalIssues(const WorldGraphData& graph) {
    // Collect unique mapIds
    std::unordered_set<uint32_t> mapIds;
    for (const auto& node : graph.GetNodes())
        mapIds.insert(node.mapId);

    int total = 0;
    for (uint32_t mapId : mapIds) {
        auto result = Validate(graph, mapId);
        total += static_cast<int>(result.issues.size());
    }
    return total;
}

} // namespace mapedit
