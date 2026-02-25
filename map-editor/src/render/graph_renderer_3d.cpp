#include "graph_renderer_3d.h"
#include "primitives_3d.h"
#include "graph_renderer.h"       // LayerVisibility
#include "../camera/camera3d.h"
#include "../data/world_graph_data.h"
#include "../editor/selection.h"
#include <imgui.h>

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

void Graph3DRenderer::Render(const Camera3D& camera, Primitives3D& prims,
                              const WorldGraphData& graph, uint32_t mapId,
                              const MultiSelection& selection, const LayerVisibility& layers) {
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

            uint32_t color = EdgeColorABGR(edge.type);
            if (selection.IsEdgeSelected(i))
                color = IM_COL32(255, 255, 0, 255); // selected: bright yellow

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

            uint32_t color = NodeColorABGR(node.type);
            float    radius = 3.0f;
            bool     selected = selection.IsNodeSelected(node.id);

            if (selected) {
                // White outer ring for selected node
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

} // namespace mapedit
