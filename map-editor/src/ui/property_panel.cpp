#include "property_panel.h"
#include "../data/world_graph_data.h"
#include "../editor/selection.h"
#include "../editor/undo_redo.h"
#include "../canvas/canvas.h"
#include "../navmesh/tile_cache.h"
#include <imgui.h>
#include <cstring>
#include <cmath>
#include <algorithm>
#include <vector>

namespace mapedit {

static constexpr float kTileSize = 533.33333f;

void PropertyPanel::Render(WorldGraphData& graph, MultiSelection& selection, uint32_t mapId,
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

    if (selection.Empty()) {
        ImGui::TextDisabled("No selection");
        ImGui::Separator();
        ImGui::Text("Nodes: %d", static_cast<int>(graph.GetNodes().size()));
        ImGui::Text("Edges: %d", static_cast<int>(graph.GetEdges().size()));
        if (graph.IsDirty())
            ImGui::TextColored(ImVec4(1,1,0,1), "Unsaved changes");
    }
    else if (selection.IsSingleNode()) {
        uint32_t nodeId = selection.SingleNodeId();
        auto* node = graph.GetNode(nodeId);
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
    else if (selection.IsSingleEdge()) {
        size_t edgeIdx = selection.SingleEdgeIndex();
        if (edgeIdx < graph.GetEdges().size()) {
            auto& edges = const_cast<std::vector<WorldEdge>&>(graph.GetEdges());
            auto& edge = edges[edgeIdx];

            ImGui::Text("Edge #%zu", edgeIdx);
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
                size_t idx = edgeIdx;
                selection.Clear();
                graph.RemoveEdge(idx);
            }
        }
    }
    else {
        // Multi-selection summary
        ImGui::Text("Selection: %zu nodes, %zu edges",
                     selection.nodes.size(), selection.edges.size());
        ImGui::Separator();

        // Bulk type change for nodes
        if (selection.HasNodes()) {
            ImGui::SeparatorText("Bulk Node Edit");
            const char* nodeTypes[] = {"waypoint","flight_master","portal","boat_zeppelin",
                                        "innkeeper","zone_boundary","dungeon"};
            static int bulkNodeType = -1;
            if (ImGui::Combo("Set Type##BulkNode", &bulkNodeType, nodeTypes, IM_ARRAYSIZE(nodeTypes))) {
                undo.Snapshot("Bulk Set Node Type");
                for (uint32_t id : selection.nodes) {
                    auto* node = graph.GetNode(id);
                    if (node) node->type = static_cast<NodeType>(bulkNodeType);
                }
                graph.MarkDirty();
                bulkNodeType = -1;
            }

            static char bulkFaction[64] = {};
            if (ImGui::InputText("Set Faction##Bulk", bulkFaction, sizeof(bulkFaction),
                                 ImGuiInputTextFlags_EnterReturnsTrue)) {
                undo.Snapshot("Bulk Set Faction");
                for (uint32_t id : selection.nodes) {
                    auto* node = graph.GetNode(id);
                    if (node) node->faction = bulkFaction;
                }
                graph.MarkDirty();
                bulkFaction[0] = '\0';
            }
        }

        // Bulk type change for edges
        if (selection.HasEdges()) {
            ImGui::SeparatorText("Bulk Edge Edit");
            const char* edgeTypes[] = {"walk", "flight", "teleport", "boat"};
            static int bulkEdgeType = -1;
            if (ImGui::Combo("Set Type##BulkEdge", &bulkEdgeType, edgeTypes, IM_ARRAYSIZE(edgeTypes))) {
                undo.Snapshot("Bulk Set Edge Type");
                auto& edges = const_cast<std::vector<WorldEdge>&>(graph.GetEdges());
                for (size_t idx : selection.edges) {
                    if (idx < edges.size())
                        edges[idx].type = static_cast<EdgeType>(bulkEdgeType);
                }
                graph.MarkDirty();
                bulkEdgeType = -1;
            }

            static int bulkBidir = -1;
            const char* bidirOpts[] = {"One-way", "Bidirectional"};
            if (ImGui::Combo("Direction##Bulk", &bulkBidir, bidirOpts, IM_ARRAYSIZE(bidirOpts))) {
                undo.Snapshot("Bulk Set Bidirectional");
                auto& edges = const_cast<std::vector<WorldEdge>&>(graph.GetEdges());
                for (size_t idx : selection.edges) {
                    if (idx < edges.size())
                        edges[idx].bidirectional = (bulkBidir == 1);
                }
                graph.MarkDirty();
                bulkBidir = -1;
            }

            if (ImGui::Button("Recalc Costs")) {
                undo.Snapshot("Recalc Edge Costs");
                auto& edges = const_cast<std::vector<WorldEdge>&>(graph.GetEdges());
                for (size_t idx : selection.edges) {
                    if (idx >= edges.size()) continue;
                    auto& edge = edges[idx];
                    auto* from = graph.GetNode(edge.fromNode);
                    auto* to = graph.GetNode(edge.toNode);
                    if (from && to) {
                        float dx = from->x - to->x;
                        float dy = from->y - to->y;
                        edge.cost = std::sqrt(dx * dx + dy * dy) / 7.0f;
                    }
                }
                graph.MarkDirty();
            }
        }

        ImGui::Separator();
        if (ImGui::Button("Delete All Selected")) {
            undo.Snapshot("Delete Selection");
            // Delete edges first (sort descending to preserve indices)
            std::vector<size_t> edgeList(selection.edges.begin(), selection.edges.end());
            std::sort(edgeList.begin(), edgeList.end(), std::greater<size_t>());
            for (size_t idx : edgeList)
                graph.RemoveEdge(idx);
            // Delete nodes (cascade-deletes their edges)
            for (uint32_t id : selection.nodes)
                graph.RemoveNode(id);
            selection.Clear();
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
