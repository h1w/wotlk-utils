#include "graph_renderer.h"
#include "../canvas/canvas.h"
#include "../data/world_graph_data.h"
#include "../editor/selection.h"
#include <imgui.h>

namespace mapedit {

static ImU32 NodeColor(NodeType type) {
    switch (type) {
    case NodeType::FlightMaster:    return IM_COL32(255, 200, 0, 255);
    case NodeType::Portal:          return IM_COL32(128, 0, 255, 255);
    case NodeType::BoatZeppelin:    return IM_COL32(0, 150, 255, 255);
    case NodeType::InnKeeper:       return IM_COL32(0, 200, 100, 255);
    case NodeType::ZoneBoundary:    return IM_COL32(200, 200, 200, 255);
    case NodeType::DungeonEntrance: return IM_COL32(200, 50, 50, 255);
    default:                        return IM_COL32(180, 180, 180, 255);
    }
}

static ImU32 EdgeColor(EdgeType type) {
    switch (type) {
    case EdgeType::Flight:  return IM_COL32(255, 200, 0, 150);
    case EdgeType::Teleport:return IM_COL32(128, 0, 255, 150);
    case EdgeType::Boat:    return IM_COL32(0, 150, 255, 150);
    default:                return IM_COL32(150, 150, 150, 100);
    }
}

// Helper: create transparent overlay window for drawing (renders above panels, below popups)
static ImDrawList* BeginGraphOverlay(const char* name, const Canvas& canvas) {
    ImGui::SetNextWindowPos(ImVec2(canvas.vpX, canvas.vpY));
    ImGui::SetNextWindowSize(ImVec2(canvas.vpW, canvas.vpH));
    ImGui::SetNextWindowBgAlpha(0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::Begin(name, nullptr,
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
        ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoInputs);
    ImGui::PopStyleVar(2);
    return ImGui::GetWindowDrawList();
}

static void EndGraphOverlay() {
    ImGui::End();
}

void GraphRenderer::Render(const Canvas& canvas, const WorldGraphData& graph,
                           uint32_t mapId, const MultiSelection& selection,
                           const LayerVisibility& layers) {
    auto* dl = BeginGraphOverlay("##GraphOverlay", canvas);

    float minX, maxX, minY, maxY;
    canvas.GetViewBounds(minX, maxX, minY, maxY);

    // Draw edges
    if (layers.showEdges) {
        for (size_t i = 0; i < graph.GetEdges().size(); ++i) {
            const auto& edge = graph.GetEdges()[i];
            const auto* from = graph.GetNode(edge.fromNode);
            const auto* to   = graph.GetNode(edge.toNode);
            if (!from || !to) continue;

            // Skip if neither endpoint is on current map
            if (from->mapId != mapId && to->mapId != mapId) continue;

            float sx1, sy1, sx2, sy2;
            canvas.WorldToScreen(from->x, from->y, sx1, sy1);
            canvas.WorldToScreen(to->x, to->y, sx2, sy2);

            ImU32 color = EdgeColor(edge.type);
            float thickness = 1.5f;

            if (selection.IsEdgeSelected(i)) {
                color = IM_COL32(255, 255, 0, 255);
                thickness = 3.0f;
            }

            dl->AddLine(ImVec2(sx1, sy1), ImVec2(sx2, sy2), color, thickness);
        }
    }

    // Draw nodes
    if (layers.showNodes) {
        for (const auto& node : graph.GetNodes()) {
            if (node.mapId != mapId) continue;

            // Frustum cull
            if (node.x < minX || node.x > maxX || node.y < minY || node.y > maxY)
                continue;

            float sx, sy;
            canvas.WorldToScreen(node.x, node.y, sx, sy);

            float radius = 6.0f;
            ImU32 color = NodeColor(node.type);

            bool selected = selection.IsNodeSelected(node.id);
            if (selected) {
                dl->AddCircleFilled(ImVec2(sx, sy), radius + 3.0f, IM_COL32(255, 255, 0, 200));
            }

            dl->AddCircleFilled(ImVec2(sx, sy), radius, color);
            dl->AddCircle(ImVec2(sx, sy), radius, IM_COL32(0, 0, 0, 200), 0, 1.5f);

            // Label
            if (layers.showLabels && canvas.zoom >= 0.04f) {
                dl->AddText(ImVec2(sx + radius + 2, sy - 6), IM_COL32(220, 220, 220, 200),
                           node.name.c_str());
            }
        }
    }

    EndGraphOverlay();
}

void GraphRenderer::RenderRoadOverlay(const Canvas& canvas, const WorldGraphData& roadGraph,
                                      uint32_t mapId) {
    auto* dl = BeginGraphOverlay("##RoadOverlay", canvas);

    float minX, maxX, minY, maxY;
    canvas.GetViewBounds(minX, maxX, minY, maxY);

    const ImU32 edgeColor = IM_COL32(200, 140, 50, 120);
    const ImU32 nodeColor = IM_COL32(180, 130, 60, 160);

    // Draw edges
    for (const auto& edge : roadGraph.GetEdges()) {
        const auto* from = roadGraph.GetNode(edge.fromNode);
        const auto* to   = roadGraph.GetNode(edge.toNode);
        if (!from || !to) continue;
        if (from->mapId != mapId && to->mapId != mapId) continue;

        float sx1, sy1, sx2, sy2;
        canvas.WorldToScreen(from->x, from->y, sx1, sy1);
        canvas.WorldToScreen(to->x, to->y, sx2, sy2);

        dl->AddLine(ImVec2(sx1, sy1), ImVec2(sx2, sy2), edgeColor, 1.0f);
    }

    // Draw nodes
    for (const auto& node : roadGraph.GetNodes()) {
        if (node.mapId != mapId) continue;
        if (node.x < minX || node.x > maxX || node.y < minY || node.y > maxY)
            continue;

        float sx, sy;
        canvas.WorldToScreen(node.x, node.y, sx, sy);
        dl->AddCircleFilled(ImVec2(sx, sy), 3.0f, nodeColor);
    }

    EndGraphOverlay();
}

} // namespace mapedit
