#include "graph_editor.h"
#include "undo_redo.h"
#include "../canvas/canvas.h"
#include "../data/world_graph_data.h"
#include <imgui.h>
#include <cmath>
#include <algorithm>

namespace mapedit {

uint32_t GraphEditor::HitTestNode(const Canvas& canvas, const WorldGraphData& graph,
                                   uint32_t mapId, float sx, float sy, float radius) {
    float bestDist = radius;
    uint32_t bestId = 0;

    for (const auto& node : graph.GetNodes()) {
        if (node.mapId != mapId) continue;
        float nsx, nsy;
        canvas.WorldToScreen(node.x, node.y, nsx, nsy);
        float dx = nsx - sx, dy = nsy - sy;
        float dist = std::sqrt(dx * dx + dy * dy);
        if (dist < bestDist) {
            bestDist = dist;
            bestId = node.id;
        }
    }
    return bestId;
}

int GraphEditor::HitTestEdge(const Canvas& canvas, const WorldGraphData& graph,
                              uint32_t mapId, float sx, float sy, float threshold) {
    float bestDist = threshold;
    int bestIdx = -1;

    for (size_t i = 0; i < graph.GetEdges().size(); ++i) {
        const auto& edge = graph.GetEdges()[i];
        const auto* from = graph.GetNode(edge.fromNode);
        const auto* to   = graph.GetNode(edge.toNode);
        if (!from || !to) continue;
        if (from->mapId != mapId && to->mapId != mapId) continue;

        float sx1, sy1, sx2, sy2;
        canvas.WorldToScreen(from->x, from->y, sx1, sy1);
        canvas.WorldToScreen(to->x, to->y, sx2, sy2);

        // Point-to-segment distance
        float dx = sx2 - sx1, dy = sy2 - sy1;
        float lenSq = dx * dx + dy * dy;
        if (lenSq < 1.0f) continue;

        float t = ((sx - sx1) * dx + (sy - sy1) * dy) / lenSq;
        t = std::clamp(t, 0.0f, 1.0f);

        float px = sx1 + t * dx, py = sy1 + t * dy;
        float ddx = sx - px, ddy = sy - py;
        float dist = std::sqrt(ddx * ddx + ddy * ddy);

        if (dist < bestDist) {
            bestDist = dist;
            bestIdx = static_cast<int>(i);
        }
    }
    return bestIdx;
}

bool GraphEditor::ProcessInput(const Canvas& canvas, WorldGraphData& graph,
                                Selection& selection, uint32_t mapId,
                                UndoContext& undo) {
    if (ImGui::GetIO().WantCaptureMouse)
        return false;

    ImVec2 mouse = ImGui::GetIO().MousePos;
    float mx = mouse.x, my = mouse.y;

    // Check if in viewport
    if (mx < canvas.vpX || mx > canvas.vpX + canvas.vpW ||
        my < canvas.vpY || my > canvas.vpY + canvas.vpH)
        return false;

    // Toggle edge mode with E key
    if (ImGui::IsKeyPressed(ImGuiKey_E) && !ImGui::GetIO().WantTextInput) {
        m_edgeMode = !m_edgeMode;
        m_edgeStartNode = 0;
    }

    // Delete selected item
    if (ImGui::IsKeyPressed(ImGuiKey_Delete) && !ImGui::GetIO().WantTextInput) {
        if (selection.IsNode()) {
            undo.Snapshot("Delete Node");
            graph.RemoveNode(selection.nodeId);
            selection.Clear();
            return true;
        }
        if (selection.IsEdge()) {
            undo.Snapshot("Delete Edge");
            graph.RemoveEdge(selection.edgeIndex);
            selection.Clear();
            return true;
        }
    }

    // Dragging selected node
    if (m_dragging && selection.IsNode()) {
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            auto* node = graph.GetNode(selection.nodeId);
            if (node) {
                if (!m_dragSnapshotTaken) {
                    ImVec2 delta = ImGui::GetMouseDragDelta(ImGuiMouseButton_Left, 2.0f);
                    if (delta.x != 0.0f || delta.y != 0.0f) {
                        undo.Snapshot("Move Node");
                        m_dragSnapshotTaken = true;
                    }
                }
                if (m_dragSnapshotTaken) {
                    float wx, wy;
                    canvas.ScreenToWorld(mx, my, wx, wy);
                    node->x = wx;
                    node->y = wy;
                    graph.MarkDirty();
                }
            }
            return true;
        } else {
            m_dragging = false;
        }
    }

    // Left click
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        // Try node hit first
        uint32_t hitNode = HitTestNode(canvas, graph, mapId, mx, my);

        if (m_edgeMode) {
            if (hitNode) {
                if (m_edgeStartNode == 0) {
                    m_edgeStartNode = hitNode;
                } else if (m_edgeStartNode != hitNode) {
                    undo.Snapshot("Create Edge");
                    WorldEdge edge;
                    edge.fromNode = m_edgeStartNode;
                    edge.toNode = hitNode;
                    edge.type = EdgeType::Walk;
                    edge.bidirectional = true;

                    // Calculate cost as distance / speed
                    auto* from = graph.GetNode(m_edgeStartNode);
                    auto* to = graph.GetNode(hitNode);
                    if (from && to) {
                        float dx = from->x - to->x;
                        float dy = from->y - to->y;
                        edge.cost = std::sqrt(dx * dx + dy * dy) / 7.0f; // ~7 yd/s run speed
                    }

                    graph.AddEdge(edge);
                    m_edgeStartNode = 0;
                }
            }
            return true;
        }

        if (hitNode) {
            selection.type = SelectionType::Node;
            selection.nodeId = hitNode;
            m_dragging = true;
            m_dragSnapshotTaken = false;
            return true;
        }

        // Try edge hit
        int hitEdge = HitTestEdge(canvas, graph, mapId, mx, my);
        if (hitEdge >= 0) {
            selection.type = SelectionType::Edge;
            selection.edgeIndex = static_cast<size_t>(hitEdge);
            return true;
        }

        // Click on empty space -- deselect
        selection.Clear();
    }

    // Double-click to add node
    if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && !m_edgeMode) {
        undo.Snapshot("Add Node");
        float wx, wy;
        canvas.ScreenToWorld(mx, my, wx, wy);

        WorldNode node;
        node.mapId = mapId;
        node.x = wx;
        node.y = wy;
        node.z = 0;
        node.name = "New Node";
        node.type = NodeType::Waypoint;

        uint32_t id = graph.AddNode(node);
        selection.type = SelectionType::Node;
        selection.nodeId = id;
        return true;
    }

    return false;
}

} // namespace mapedit
