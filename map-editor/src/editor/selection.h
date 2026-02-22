#pragma once
#include <cstdint>
#include <cstddef>

namespace mapedit {

enum class SelectionType { None, Node, Edge };

struct Selection {
    SelectionType type = SelectionType::None;
    uint32_t nodeId = 0;     // valid when type==Node
    size_t edgeIndex = 0;    // valid when type==Edge

    void Clear() { type = SelectionType::None; nodeId = 0; edgeIndex = 0; }
    bool IsNode() const { return type == SelectionType::Node; }
    bool IsEdge() const { return type == SelectionType::Edge; }
    bool HasSelection() const { return type != SelectionType::None; }
};

} // namespace mapedit
