#include "property_panel.h"
#include "../data/world_graph_data.h"
#include "../editor/selection.h"
#include "../editor/undo_redo.h"
#include "../canvas/canvas.h"
#include "../navmesh/tile_cache.h"
#include <imgui.h>
#include <cstring>
#include <cmath>

namespace mapedit {

static constexpr float kTileSize = 533.33333f;

void PropertyPanel::Render(WorldGraphData& graph, Selection& selection, uint32_t mapId,
                           const Canvas& canvas, TileCache& tileCache, UndoContext& undo) {
    ImGuiViewport* vp = ImGui::GetMainViewport();
    float panelW = 280.0f;
    float toolbarH = 36.0f;
    float statusBarH = 28.0f;
    float panelH = vp->WorkSize.y - toolbarH - statusBarH;

    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x - panelW,
                                   vp->WorkPos.y + toolbarH));
    ImGui::SetNextWindowSize(ImVec2(panelW, panelH));

    ImGui::Begin("Properties", nullptr,
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse);

    if (!selection.HasSelection()) {
        ImGui::TextDisabled("No selection");
        ImGui::Separator();
        ImGui::Text("Nodes: %d", static_cast<int>(graph.GetNodes().size()));
        ImGui::Text("Edges: %d", static_cast<int>(graph.GetEdges().size()));
        if (graph.IsDirty())
            ImGui::TextColored(ImVec4(1,1,0,1), "Unsaved changes");
    }
    else if (selection.IsNode()) {
        auto* node = graph.GetNode(selection.nodeId);
        if (node) {
            ImGui::Text("Node #%u", node->id);
            ImGui::Separator();

            char nameBuf[256];
            strncpy(nameBuf, node->name.c_str(), sizeof(nameBuf) - 1);
            nameBuf[sizeof(nameBuf) - 1] = '\0';
            if (ImGui::InputText("Name", nameBuf, sizeof(nameBuf))) {
                undo.SnapshotIfNeeded("Edit Node Name");
                node->name = nameBuf;
                graph.MarkDirty();
            }

            {
                float prevX = node->x;
                if (ImGui::InputFloat("X", &node->x, 1.0f, 10.0f, "%.1f")) {
                    float newX = node->x; node->x = prevX;
                    undo.SnapshotIfNeeded("Edit Node X");
                    node->x = newX;
                    graph.MarkDirty();
                }
            }
            {
                float prevY = node->y;
                if (ImGui::InputFloat("Y", &node->y, 1.0f, 10.0f, "%.1f")) {
                    float newY = node->y; node->y = prevY;
                    undo.SnapshotIfNeeded("Edit Node Y");
                    node->y = newY;
                    graph.MarkDirty();
                }
            }
            {
                float prevZ = node->z;
                if (ImGui::InputFloat("Z", &node->z, 1.0f, 10.0f, "%.1f")) {
                    float newZ = node->z; node->z = prevZ;
                    undo.SnapshotIfNeeded("Edit Node Z");
                    node->z = newZ;
                    graph.MarkDirty();
                }
            }

            int mapIdInt = static_cast<int>(node->mapId);
            if (ImGui::InputInt("Map ID", &mapIdInt)) {
                undo.SnapshotIfNeeded("Edit Map ID");
                node->mapId = static_cast<uint32_t>(mapIdInt);
                graph.MarkDirty();
            }

            const char* nodeTypes[] = {"waypoint","flight_master","portal","boat_zeppelin",
                                        "innkeeper","zone_boundary","dungeon"};
            int typeIdx = static_cast<int>(node->type);
            if (ImGui::Combo("Type", &typeIdx, nodeTypes, IM_ARRAYSIZE(nodeTypes))) {
                undo.Snapshot("Edit Node Type");
                node->type = static_cast<NodeType>(typeIdx);
                graph.MarkDirty();
            }

            char factionBuf[64];
            strncpy(factionBuf, node->faction.c_str(), sizeof(factionBuf) - 1);
            factionBuf[sizeof(factionBuf) - 1] = '\0';
            if (ImGui::InputText("Faction", factionBuf, sizeof(factionBuf))) {
                undo.SnapshotIfNeeded("Edit Node Faction");
                node->faction = factionBuf;
                graph.MarkDirty();
            }

            ImGui::Separator();
            if (ImGui::Button("Delete Node")) {
                undo.Snapshot("Delete Node");
                uint32_t id = node->id;
                selection.Clear();
                graph.RemoveNode(id);
            }
        }
    }
    else if (selection.IsEdge()) {
        if (selection.edgeIndex < graph.GetEdges().size()) {
            // Need mutable access
            auto& edges = const_cast<std::vector<WorldEdge>&>(graph.GetEdges());
            auto& edge = edges[selection.edgeIndex];

            ImGui::Text("Edge #%zu", selection.edgeIndex);
            ImGui::Separator();

            ImGui::Text("From: %u", edge.fromNode);
            ImGui::Text("To:   %u", edge.toNode);

            const char* edgeTypes[] = {"walk", "flight", "teleport", "boat"};
            int typeIdx = static_cast<int>(edge.type);
            if (ImGui::Combo("Type", &typeIdx, edgeTypes, IM_ARRAYSIZE(edgeTypes))) {
                undo.Snapshot("Edit Edge Type");
                edge.type = static_cast<EdgeType>(typeIdx);
                graph.MarkDirty();
            }

            {
                float prevCost = edge.cost;
                if (ImGui::InputFloat("Cost", &edge.cost, 1.0f, 10.0f, "%.1f")) {
                    float newCost = edge.cost; edge.cost = prevCost;
                    undo.SnapshotIfNeeded("Edit Edge Cost");
                    edge.cost = newCost;
                    graph.MarkDirty();
                }
            }

            {
                bool prevBidir = edge.bidirectional;
                if (ImGui::Checkbox("Bidirectional", &edge.bidirectional)) {
                    bool newBidir = edge.bidirectional;
                    edge.bidirectional = prevBidir;
                    undo.Snapshot("Edit Bidirectional");
                    edge.bidirectional = newBidir;
                    graph.MarkDirty();
                }
            }

            ImGui::Separator();
            if (ImGui::Button("Delete Edge")) {
                undo.Snapshot("Delete Edge");
                size_t idx = selection.edgeIndex;
                selection.Clear();
                graph.RemoveEdge(idx);
            }
        }
    }

    // --- Navmesh Tile Info (always shown at bottom) ---
    ImGui::Separator();
    ImGui::SeparatorText("Navmesh Tile");

    auto& io = ImGui::GetIO();
    float wx, wy;
    canvas.ScreenToWorld(io.MousePos.x, io.MousePos.y, wx, wy);
    int tileX = 31 - static_cast<int>(std::floor(wx / kTileSize));
    int tileY = 31 - static_cast<int>(std::floor(wy / kTileSize));

    auto info = tileCache.GetTileInfo(tileX, tileY);
    if (info.valid) {
        ImGui::Text("Tile [%d, %d]", info.tileX, info.tileY);
        ImGui::Text("Detour [%d, %d]", info.detourX, info.detourY);
        ImGui::Separator();
        ImGui::Text("Polygons:     %d", info.polyCount);
        ImGui::Text("Vertices:     %d", info.vertCount);
        ImGui::Text("Detail Tris:  %d", info.detailTriCount);
        ImGui::Text("Detail Verts: %d", info.detailVertCount);
        ImGui::Text("BV Nodes:     %d", info.bvNodeCount);
        ImGui::Text("Off-mesh:     %d", info.offMeshConCount);
        ImGui::Text("Max Links:    %d", info.maxLinkCount);
        ImGui::Separator();
        ImGui::Text("Walkable H:   %.2f", info.walkableHeight);
        ImGui::Text("Walkable R:   %.2f", info.walkableRadius);
        ImGui::Text("Walkable Climb: %.2f", info.walkableClimb);
        ImGui::Separator();
        ImGui::Text("Render Tris:  %d", info.renderTriCount);
        ImGui::Text("In Navmesh:   %s", info.inNavmesh ? "Yes" : "No");
        ImGui::Text("In Cache:     %s", info.inRenderCache ? "Yes" : "No");
    } else {
        ImGui::TextDisabled("No tile data");
    }

    ImGui::End();
}

} // namespace mapedit
