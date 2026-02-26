#include "graph_renderer.h"
#include "../canvas/canvas.h"
#include "../data/world_graph_data.h"
#include "../editor/selection.h"
#include "../editor/graph_validator.h"
#include <imgui.h>
#include <cmath>

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

// 10-color palette for component coloring (HSL evenly spaced, saturated, medium lightness)
static ImU32 ComponentColor(int componentIdx, int alpha = 200) {
    static const ImU32 kPalette[] = {
        IM_COL32(230,  50,  50, 255),  // red
        IM_COL32( 50, 180,  50, 255),  // green
        IM_COL32( 50, 100, 230, 255),  // blue
        IM_COL32(230, 180,  50, 255),  // yellow
        IM_COL32(200,  50, 200, 255),  // magenta
        IM_COL32( 50, 200, 200, 255),  // cyan
        IM_COL32(255, 130,  50, 255),  // orange
        IM_COL32(130,  50, 255, 255),  // violet
        IM_COL32( 50, 230, 130, 255),  // mint
        IM_COL32(230,  50, 130, 255),  // pink
    };
    static constexpr int kPaletteSize = 10;

    if (componentIdx <= 0) return IM_COL32(180, 180, 180, static_cast<uint8_t>(alpha));

    ImU32 base = kPalette[(componentIdx - 1) % kPaletteSize];
    // Replace alpha
    return (base & 0x00FFFFFF) | (static_cast<uint32_t>(alpha) << 24);
}

// Draw a dashed line between two screen points
static void DrawDashedLine(ImDrawList* dl, ImVec2 a, ImVec2 b, ImU32 color, float thickness,
                           float dashLen = 8.0f, float gapLen = 4.0f) {
    float dx = b.x - a.x, dy = b.y - a.y;
    float len = std::sqrt(dx * dx + dy * dy);
    if (len < 1.0f) return;
    float ux = dx / len, uy = dy / len;

    float pos = 0.0f;
    while (pos < len) {
        float segEnd = pos + dashLen;
        if (segEnd > len) segEnd = len;
        dl->AddLine(
            ImVec2(a.x + ux * pos, a.y + uy * pos),
            ImVec2(a.x + ux * segEnd, a.y + uy * segEnd),
            color, thickness);
        pos = segEnd + gapLen;
    }
}

void GraphRenderer::RenderIssueOverlay(const Canvas& canvas, const WorldGraphData& graph,
                                       uint32_t mapId, const ValidationResult& validation,
                                       float gapMaxDistance) {
    if (validation.componentCount <= 1 && validation.gaps.empty())
        return;

    auto* dl = BeginGraphOverlay("##IssueOverlay", canvas);

    // Component coloring: only when multiple components exist
    if (validation.componentCount > 1) {
        // Color edges by component (use fromNode's component)
        for (const auto& edge : graph.GetEdges()) {
            const auto* from = graph.GetNode(edge.fromNode);
            const auto* to = graph.GetNode(edge.toNode);
            if (!from || !to) continue;
            if (from->mapId != mapId && to->mapId != mapId) continue;

            auto itFrom = validation.nodeComponent.find(edge.fromNode);
            auto itTo = validation.nodeComponent.find(edge.toNode);
            if (itFrom == validation.nodeComponent.end()) continue;

            int comp = itFrom->second;
            if (comp == 0) continue; // Largest component keeps default colors

            ImU32 color = ComponentColor(comp, 120);

            float sx1, sy1, sx2, sy2;
            canvas.WorldToScreen(from->x, from->y, sx1, sy1);
            canvas.WorldToScreen(to->x, to->y, sx2, sy2);

            dl->AddLine(ImVec2(sx1, sy1), ImVec2(sx2, sy2), color, 2.5f);
        }

        // Color nodes by component
        float minX, maxX, minY, maxY;
        canvas.GetViewBounds(minX, maxX, minY, maxY);

        for (const auto& node : graph.GetNodes()) {
            if (node.mapId != mapId) continue;
            if (node.x < minX || node.x > maxX || node.y < minY || node.y > maxY)
                continue;

            auto it = validation.nodeComponent.find(node.id);
            if (it == validation.nodeComponent.end()) continue;
            int comp = it->second;
            if (comp == 0) continue; // Largest component keeps default colors

            float sx, sy;
            canvas.WorldToScreen(node.x, node.y, sx, sy);

            ImU32 ringColor = ComponentColor(comp, 220);
            dl->AddCircle(ImVec2(sx, sy), 8.0f, ringColor, 0, 2.0f);
        }
    }

    // Gap markers: dashed lines between closest nodes of different components
    for (const auto& gap : validation.gaps) {
        if (gap.distance > gapMaxDistance)
            continue;

        const auto* nodeA = graph.GetNode(gap.nodeA);
        const auto* nodeB = graph.GetNode(gap.nodeB);
        if (!nodeA || !nodeB) continue;

        float sx1, sy1, sx2, sy2;
        canvas.WorldToScreen(nodeA->x, nodeA->y, sx1, sy1);
        canvas.WorldToScreen(nodeB->x, nodeB->y, sx2, sy2);

        ImU32 gapColor = IM_COL32(255, 100, 30, 200);
        DrawDashedLine(dl, ImVec2(sx1, sy1), ImVec2(sx2, sy2), gapColor, 2.0f);

        // Warning triangle at midpoint
        float mx = (sx1 + sx2) * 0.5f;
        float my = (sy1 + sy2) * 0.5f;
        float triSize = 7.0f;
        ImVec2 p1(mx, my - triSize);
        ImVec2 p2(mx - triSize * 0.866f, my + triSize * 0.5f);
        ImVec2 p3(mx + triSize * 0.866f, my + triSize * 0.5f);
        dl->AddTriangleFilled(p1, p2, p3, IM_COL32(255, 180, 0, 220));
        dl->AddTriangle(p1, p2, p3, IM_COL32(0, 0, 0, 180), 1.5f);

        // "!" inside triangle
        dl->AddText(ImVec2(mx - 2.5f, my - 5.0f), IM_COL32(0, 0, 0, 220), "!");
    }

    EndGraphOverlay();
}

} // namespace mapedit
