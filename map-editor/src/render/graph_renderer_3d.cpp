#include "graph_renderer_3d.h"
#include "primitives_3d.h"
#include "graph_renderer.h"       // LayerVisibility
#include "../camera/camera3d.h"
#include "../data/world_graph_data.h"
#include "../editor/selection.h"
#include "../editor/graph_validator.h"
#include <imgui.h>
#include <cmath>

namespace mapedit {

// ---------------------------------------------------------------------------
// Color helpers — mirror exactly the 2D GraphRenderer color scheme
// ---------------------------------------------------------------------------

static uint32_t NodeColorABGR(NodeType type) {
    // IM_COL32 is ABGR in memory (A<<24 | B<<16 | G<<8 | R<<0)
    switch (type) {
    case NodeType::FlightMaster:    return IM_COL32(255, 200,   0, 255); // gold
    case NodeType::Portal:          return IM_COL32(128,   0, 255, 255); // purple
    case NodeType::BoatZeppelin:    return IM_COL32(  0, 150, 255, 255); // blue
    case NodeType::InnKeeper:       return IM_COL32(  0, 200, 100, 255); // green
    case NodeType::ZoneBoundary:    return IM_COL32(200, 200, 200, 255); // light grey
    case NodeType::DungeonEntrance: return IM_COL32(200,  50,  50, 255); // red
    default:                        return IM_COL32(180, 180, 180, 255); // grey
    }
}

static uint32_t EdgeColorABGR(EdgeType type) {
    switch (type) {
    case EdgeType::Flight:   return IM_COL32(255, 200,   0, 150); // gold
    case EdgeType::Teleport: return IM_COL32(128,   0, 255, 150); // purple
    case EdgeType::Boat:     return IM_COL32(  0, 150, 255, 150); // blue
    default:                 return IM_COL32(150, 150, 150, 100); // grey
    }
}

// ---------------------------------------------------------------------------
// Render
// ---------------------------------------------------------------------------

// Apply dimAlpha to the alpha channel of an ABGR color
static uint32_t ApplyDim(uint32_t color, float dimAlpha) {
    if (dimAlpha >= 1.0f) return color;
    uint32_t a = (color >> 24) & 0xFF;
    a = static_cast<uint32_t>(a * dimAlpha);
    return (color & 0x00FFFFFF) | (a << 24);
}

void Graph3DRenderer::Render(const Camera3D& camera, Primitives3D& prims,
                              const WorldGraphData& graph, uint32_t mapId,
                              const MultiSelection& selection, const LayerVisibility& layers,
                              float dimAlpha) {
    auto* fgDL = ImGui::GetForegroundDrawList();

    // Draw edges first so nodes render on top
    if (layers.showEdges) {
        for (size_t i = 0; i < graph.GetEdges().size(); ++i) {
            const auto& edge = graph.GetEdges()[i];
            const auto* from = graph.GetNode(edge.fromNode);
            const auto* to   = graph.GetNode(edge.toNode);
            if (!from || !to) continue;

            // Skip if neither endpoint is on the current map
            if (from->mapId != mapId && to->mapId != mapId) continue;

            uint32_t color = ApplyDim(EdgeColorABGR(edge.type), dimAlpha);
            if (selection.IsEdgeSelected(i))
                color = IM_COL32(255, 255, 0, 255); // selected: bright yellow (not dimmed)

            // Elevate slightly above ground to reduce z-fighting with navmesh
            prims.AddLine(from->x, from->y, from->z + 0.5f,
                          to->x,   to->y,   to->z   + 0.5f,
                          color);
        }
    }

    // Draw nodes
    if (layers.showNodes) {
        for (const auto& node : graph.GetNodes()) {
            if (node.mapId != mapId) continue;

            uint32_t color = ApplyDim(NodeColorABGR(node.type), dimAlpha);
            float    radius = 2.1f;
            bool     selected = selection.IsNodeSelected(node.id);

            if (selected) {
                // White outer ring for selected node (not dimmed)
                prims.AddCircle(node.x, node.y, node.z + 0.5f,
                                radius + 2.0f, IM_COL32(255, 255, 255, 220), 24);
            }

            prims.AddCircle(node.x, node.y, node.z + 0.5f, radius, color, 16);

            // Label: project to screen and draw with ImGui
            if (layers.showLabels && !node.name.empty()) {
                float sx, sy;
                if (camera.WorldToScreen(node.x, node.y, node.z + 1.5f, sx, sy)) {
                    fgDL->AddText(ImVec2(sx + 8.0f, sy - 6.0f),
                                  IM_COL32(220, 220, 220, 200),
                                  node.name.c_str());
                }
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Component color palette (duplicated from graph_renderer.cpp — small static helper)
// ---------------------------------------------------------------------------
static uint32_t ComponentColor3D(int componentIdx, int alpha = 200) {
    static const uint32_t kPalette[] = {
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

    uint32_t base = kPalette[(componentIdx - 1) % kPaletteSize];
    return (base & 0x00FFFFFF) | (static_cast<uint32_t>(alpha) << 24);
}

// Draw a 3D dashed line using segmented AddLine calls
static void DrawDashedLine3D(Primitives3D& prims,
                             float x1, float y1, float z1,
                             float x2, float y2, float z2,
                             uint32_t color,
                             float dashLen = 4.0f, float gapLen = 3.0f) {
    float dx = x2 - x1, dy = y2 - y1, dz = z2 - z1;
    float len = std::sqrt(dx * dx + dy * dy + dz * dz);
    if (len < 0.01f) return;
    float ux = dx / len, uy = dy / len, uz = dz / len;

    float pos = 0.0f;
    while (pos < len) {
        float segEnd = pos + dashLen;
        if (segEnd > len) segEnd = len;
        prims.AddLine(x1 + ux * pos, y1 + uy * pos, z1 + uz * pos,
                      x1 + ux * segEnd, y1 + uy * segEnd, z1 + uz * segEnd,
                      color);
        pos = segEnd + gapLen;
    }
}

void Graph3DRenderer::RenderIssueOverlay(const Camera3D& camera, Primitives3D& prims,
                                          const WorldGraphData& graph, uint32_t mapId,
                                          const ValidationResult& validation,
                                          float gapMaxDistance) {
    if (validation.componentCount <= 1 && validation.gaps.empty())
        return;

    // Component coloring: only when multiple components exist
    if (validation.componentCount > 1) {
        // Color edges by component (use fromNode's component)
        for (const auto& edge : graph.GetEdges()) {
            const auto* from = graph.GetNode(edge.fromNode);
            const auto* to = graph.GetNode(edge.toNode);
            if (!from || !to) continue;
            if (from->mapId != mapId && to->mapId != mapId) continue;

            auto itFrom = validation.nodeComponent.find(edge.fromNode);
            if (itFrom == validation.nodeComponent.end()) continue;

            int comp = itFrom->second;
            if (comp == 0) continue; // Largest component keeps default colors

            uint32_t color = ComponentColor3D(comp, 120);
            prims.AddLine(from->x, from->y, from->z + 0.7f,
                          to->x,   to->y,   to->z   + 0.7f, color);
        }

        // Color nodes by component
        for (const auto& node : graph.GetNodes()) {
            if (node.mapId != mapId) continue;

            auto it = validation.nodeComponent.find(node.id);
            if (it == validation.nodeComponent.end()) continue;
            int comp = it->second;
            if (comp == 0) continue;

            uint32_t ringColor = ComponentColor3D(comp, 220);
            prims.AddCircle(node.x, node.y, node.z + 0.5f, 3.5f, ringColor, 24);
        }
    }

    // Gap markers: dashed lines between closest nodes of different components
    const uint32_t gapColor = IM_COL32(255, 100, 30, 200);
    for (const auto& gap : validation.gaps) {
        if (gap.distance > gapMaxDistance)
            continue;

        const auto* nodeA = graph.GetNode(gap.nodeA);
        const auto* nodeB = graph.GetNode(gap.nodeB);
        if (!nodeA || !nodeB) continue;

        DrawDashedLine3D(prims,
                         nodeA->x, nodeA->y, nodeA->z + 0.7f,
                         nodeB->x, nodeB->y, nodeB->z + 0.7f,
                         gapColor);
    }
}

} // namespace mapedit
