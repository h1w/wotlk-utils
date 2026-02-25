#pragma once
#include <cstdint>
#include <cstddef>
#include <unordered_set>

namespace mapedit {

enum class SelectionType { None, Node, Edge };

struct MultiSelection {
    std::unordered_set<uint32_t> nodes;
    std::unordered_set<size_t>   edges;

    // --- Queries ---
    bool Empty()    const { return nodes.empty() && edges.empty(); }
    bool HasNodes() const { return !nodes.empty(); }
    bool HasEdges() const { return !edges.empty(); }
    bool IsNodeSelected(uint32_t id) const { return nodes.count(id) > 0; }
    bool IsEdgeSelected(size_t idx)  const { return edges.count(idx) > 0; }
    size_t Count() const { return nodes.size() + edges.size(); }

    // --- Backwards compat (single selection) ---
    bool IsSingleNode() const { return nodes.size() == 1 && edges.empty(); }
    bool IsSingleEdge() const { return edges.size() == 1 && nodes.empty(); }
    uint32_t SingleNodeId() const { return IsSingleNode() ? *nodes.begin() : 0; }
    size_t   SingleEdgeIndex() const { return IsSingleEdge() ? *edges.begin() : 0; }

    // --- Mutations ---
    void Clear() { nodes.clear(); edges.clear(); }

    void SelectNode(uint32_t id)   { nodes.insert(id); }
    void DeselectNode(uint32_t id) { nodes.erase(id); }
    void ToggleNode(uint32_t id) {
        if (nodes.count(id)) nodes.erase(id);
        else nodes.insert(id);
    }
    void SetSingleNode(uint32_t id) { Clear(); nodes.insert(id); }

    void SelectEdge(size_t idx)   { edges.insert(idx); }
    void DeselectEdge(size_t idx) { edges.erase(idx); }
    void ToggleEdge(size_t idx) {
        if (edges.count(idx)) edges.erase(idx);
        else edges.insert(idx);
    }
    void SetSingleEdge(size_t idx) { Clear(); edges.insert(idx); }
};

} // namespace mapedit
