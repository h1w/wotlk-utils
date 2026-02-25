#include "graph_editor.h"
#include "undo_redo.h"
#include "../canvas/canvas.h"
#include "../data/world_graph_data.h"
#include <imgui.h>
#include <cmath>
#include <algorithm>
#include <vector>
#include <unordered_map>
#include <unordered_set>

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

// Auto-select edges whose BOTH endpoints are in the selection
static void AutoSelectEdges(MultiSelection& selection, const WorldGraphData& graph) {
    for (size_t i = 0; i < graph.GetEdges().size(); ++i) {
        const auto& edge = graph.GetEdges()[i];
        if (selection.IsNodeSelected(edge.fromNode) && selection.IsNodeSelected(edge.toNode))
            selection.SelectEdge(i);
    }
}

bool GraphEditor::RenderPopups(const Canvas& canvas, WorldGraphData& graph,
                                MultiSelection& selection, uint32_t mapId,
                                UndoContext& undo) {
    bool handled = false;

    // Invisible host window provides ImGui window scope for popup API.
    // OpenPopup/BeginPopup require a parent window context to track popup state.
    ImGui::SetNextWindowBgAlpha(0.0f);
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::Begin("##GraphPopupHost", nullptr,
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
        ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus |
        ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoMouseInputs);
    ImGui::PopStyleVar(2);

    // Deferred open: flag set by ProcessInput on right-click
    if (m_wantOpenContextMenu) {
        ImGui::OpenPopup("GraphContextMenu");
        m_wantOpenContextMenu = false;
    }

    // Context menu popup
    if (ImGui::BeginPopup("GraphContextMenu")) {
        handled = true;

        // Add Node Here
        if (ImGui::MenuItem("Add Node Here")) {
            undo.Snapshot("Add Node");
            WorldNode node;
            node.mapId = mapId;
            node.x = m_contextMenuWx;
            node.y = m_contextMenuWy;
            node.z = 0;
            node.name = "";
            node.type = NodeType::Waypoint;
            uint32_t id = graph.AddNode(node);
            selection.SetSingleNode(id);
        }

        // Connect Nodes (exactly 2 selected)
        if (selection.nodes.size() == 2 && ImGui::MenuItem("Connect Nodes")) {
            auto it = selection.nodes.begin();
            uint32_t a = *it++; uint32_t b = *it;
            // Check for duplicate
            bool duplicate = false;
            for (const auto& edge : graph.GetEdges()) {
                if ((edge.fromNode == a && edge.toNode == b) ||
                    (edge.fromNode == b && edge.toNode == a)) {
                    duplicate = true;
                    break;
                }
            }
            if (!duplicate) {
                undo.Snapshot("Connect Nodes");
                WorldEdge edge;
                edge.fromNode = a;
                edge.toNode = b;
                edge.type = EdgeType::Walk;
                edge.bidirectional = true;
                auto* from = graph.GetNode(a);
                auto* to = graph.GetNode(b);
                if (from && to) {
                    float dx = from->x - to->x;
                    float dy = from->y - to->y;
                    edge.cost = std::sqrt(dx * dx + dy * dy) / 7.0f;
                }
                graph.AddEdge(edge);
            }
        }

        // Split Edge (edge was hit)
        if (m_contextHitEdge >= 0 && ImGui::MenuItem("Split Edge")) {
            size_t idx = static_cast<size_t>(m_contextHitEdge);
            if (idx < graph.GetEdges().size()) {
                const auto& edge = graph.GetEdges()[idx];
                auto* from = graph.GetNode(edge.fromNode);
                auto* to   = graph.GetNode(edge.toNode);
                if (from && to) {
                    undo.Snapshot("Split Edge");
                    WorldNode mid;
                    mid.mapId = from->mapId;
                    mid.x = (from->x + to->x) * 0.5f;
                    mid.y = (from->y + to->y) * 0.5f;
                    mid.z = (from->z + to->z) * 0.5f;
                    mid.name = "";
                    mid.type = NodeType::Waypoint;

                    EdgeType etype = edge.type;
                    bool bidir = edge.bidirectional;
                    uint32_t fromId = edge.fromNode;
                    uint32_t toId = edge.toNode;

                    uint32_t midId = graph.AddNode(mid);

                    // Remove original edge
                    graph.RemoveEdge(idx);

                    // Create two new edges
                    auto calcCost = [&](uint32_t a, uint32_t b) -> float {
                        auto* na = graph.GetNode(a);
                        auto* nb = graph.GetNode(b);
                        if (!na || !nb) return 0;
                        float dx = na->x - nb->x;
                        float dy = na->y - nb->y;
                        return std::sqrt(dx * dx + dy * dy) / 7.0f;
                    };

                    WorldEdge e1;
                    e1.fromNode = fromId; e1.toNode = midId;
                    e1.type = etype; e1.bidirectional = bidir;
                    e1.cost = calcCost(fromId, midId);
                    graph.AddEdge(e1);

                    WorldEdge e2;
                    e2.fromNode = midId; e2.toNode = toId;
                    e2.type = etype; e2.bidirectional = bidir;
                    e2.cost = calcCost(midId, toId);
                    graph.AddEdge(e2);

                    selection.SetSingleNode(midId);
                }
            }
        }

        // Merge Nodes (2+ selected)
        if (selection.nodes.size() >= 2 && ImGui::MenuItem("Merge Nodes")) {
            undo.Snapshot("Merge Nodes");
            // Compute centroid
            float cx = 0, cy = 0, cz = 0;
            int count = 0;
            for (uint32_t id : selection.nodes) {
                auto* n = graph.GetNode(id);
                if (n) { cx += n->x; cy += n->y; cz += n->z; ++count; }
            }
            if (count > 0) {
                cx /= count; cy /= count; cz /= count;
                // Pick first node as survivor
                uint32_t survivorId = *selection.nodes.begin();
                auto* survivor = graph.GetNode(survivorId);
                if (survivor) {
                    survivor->x = cx;
                    survivor->y = cy;
                    survivor->z = cz;
                }
                // Remap edges from other nodes to survivor
                auto& edges = const_cast<std::vector<WorldEdge>&>(graph.GetEdges());
                for (auto& edge : edges) {
                    if (selection.IsNodeSelected(edge.fromNode) && edge.fromNode != survivorId)
                        edge.fromNode = survivorId;
                    if (selection.IsNodeSelected(edge.toNode) && edge.toNode != survivorId)
                        edge.toNode = survivorId;
                }
                // Remove self-loops and duplicates
                for (int i = static_cast<int>(edges.size()) - 1; i >= 0; --i) {
                    if (edges[i].fromNode == edges[i].toNode) {
                        graph.RemoveEdge(static_cast<size_t>(i));
                        continue;
                    }
                    // Check for duplicates
                    for (int j = 0; j < i; ++j) {
                        if ((edges[j].fromNode == edges[i].fromNode && edges[j].toNode == edges[i].toNode) ||
                            (edges[j].fromNode == edges[i].toNode && edges[j].toNode == edges[i].fromNode)) {
                            graph.RemoveEdge(static_cast<size_t>(i));
                            break;
                        }
                    }
                }
                // Remove merged nodes
                std::vector<uint32_t> toRemove;
                for (uint32_t id : selection.nodes) {
                    if (id != survivorId) toRemove.push_back(id);
                }
                for (uint32_t id : toRemove)
                    graph.RemoveNode(id);
                selection.SetSingleNode(survivorId);
                graph.MarkDirty();
            }
        }

        // Straighten Path (3+ connected chain nodes)
        if (selection.nodes.size() >= 3 && ImGui::MenuItem("Straighten Path")) {
            // BFS to find chain order: build adjacency from selected edges
            std::unordered_map<uint32_t, std::vector<uint32_t>> adj;
            for (const auto& edge : graph.GetEdges()) {
                bool fromSel = selection.IsNodeSelected(edge.fromNode);
                bool toSel = selection.IsNodeSelected(edge.toNode);
                if (fromSel && toSel) {
                    adj[edge.fromNode].push_back(edge.toNode);
                    adj[edge.toNode].push_back(edge.fromNode);
                }
            }
            // Find endpoints (degree 1 in the subgraph)
            std::vector<uint32_t> endpoints;
            for (uint32_t id : selection.nodes) {
                if (adj[id].size() == 1) endpoints.push_back(id);
            }
            if (endpoints.size() == 2) {
                // Walk chain from endpoints[0] to endpoints[1]
                std::vector<uint32_t> chain;
                std::unordered_set<uint32_t> visited;
                uint32_t cur = endpoints[0];
                while (cur != 0) {
                    chain.push_back(cur);
                    visited.insert(cur);
                    uint32_t next = 0;
                    for (uint32_t nb : adj[cur]) {
                        if (!visited.count(nb)) { next = nb; break; }
                    }
                    cur = next;
                }
                if (chain.size() == selection.nodes.size() && chain.back() == endpoints[1]) {
                    undo.Snapshot("Straighten Path");
                    auto* startNode = graph.GetNode(chain.front());
                    auto* endNode = graph.GetNode(chain.back());
                    if (startNode && endNode) {
                        for (size_t i = 1; i + 1 < chain.size(); ++i) {
                            float t = static_cast<float>(i) / static_cast<float>(chain.size() - 1);
                            auto* node = graph.GetNode(chain[i]);
                            if (node) {
                                node->x = startNode->x + t * (endNode->x - startNode->x);
                                node->y = startNode->y + t * (endNode->y - startNode->y);
                                node->z = startNode->z + t * (endNode->z - startNode->z);
                            }
                        }
                        graph.MarkDirty();
                    }
                }
            }
        }

        // Delete Selected
        if (!selection.Empty() && ImGui::MenuItem("Delete Selected")) {
            undo.Snapshot("Delete Selection");
            std::vector<size_t> edgeList(selection.edges.begin(), selection.edges.end());
            std::sort(edgeList.begin(), edgeList.end(), std::greater<size_t>());
            for (size_t idx : edgeList)
                graph.RemoveEdge(idx);
            std::vector<uint32_t> nodeList(selection.nodes.begin(), selection.nodes.end());
            for (uint32_t id : nodeList)
                graph.RemoveNode(id);
            selection.Clear();
        }

        // Auto-Connect Nearby Endpoints
        if (ImGui::MenuItem("Auto-Connect Endpoints...")) {
            // Find endpoint nodes (degree <= 1) on current map
            std::unordered_map<uint32_t, int> degree;
            for (const auto& node : graph.GetNodes()) {
                if (node.mapId == mapId) degree[node.id] = 0;
            }
            for (const auto& edge : graph.GetEdges()) {
                if (degree.count(edge.fromNode)) degree[edge.fromNode]++;
                if (degree.count(edge.toNode)) degree[edge.toNode]++;
            }
            std::vector<uint32_t> endpoints;
            for (auto& [id, deg] : degree) {
                if (deg <= 1) endpoints.push_back(id);
            }
            // Connect nearby pairs (< 100 world units)
            float maxDist = 100.0f;
            int connected = 0;
            bool snapshotTaken = false;
            for (size_t i = 0; i < endpoints.size(); ++i) {
                for (size_t j = i + 1; j < endpoints.size(); ++j) {
                    auto* a = graph.GetNode(endpoints[i]);
                    auto* b = graph.GetNode(endpoints[j]);
                    if (!a || !b) continue;
                    float dx = a->x - b->x;
                    float dy = a->y - b->y;
                    float dist = std::sqrt(dx * dx + dy * dy);
                    if (dist > maxDist) continue;
                    // Check no edge exists
                    bool exists = false;
                    for (const auto& edge : graph.GetEdges()) {
                        if ((edge.fromNode == endpoints[i] && edge.toNode == endpoints[j]) ||
                            (edge.fromNode == endpoints[j] && edge.toNode == endpoints[i])) {
                            exists = true; break;
                        }
                    }
                    if (exists) continue;
                    if (!snapshotTaken) {
                        undo.Snapshot("Auto-Connect Endpoints");
                        snapshotTaken = true;
                    }
                    WorldEdge edge;
                    edge.fromNode = endpoints[i];
                    edge.toNode = endpoints[j];
                    edge.type = EdgeType::Walk;
                    edge.bidirectional = true;
                    edge.cost = dist / 7.0f;
                    graph.AddEdge(edge);
                    ++connected;
                }
            }
        }

        // Validate Graph
        if (ImGui::MenuItem("Validate Graph...")) {
            // Compute degree for all nodes on this map
            std::unordered_map<uint32_t, int> degree;
            for (const auto& node : graph.GetNodes()) {
                if (node.mapId == mapId) degree[node.id] = 0;
            }
            for (const auto& edge : graph.GetEdges()) {
                if (degree.count(edge.fromNode)) degree[edge.fromNode]++;
                if (degree.count(edge.toNode)) degree[edge.toNode]++;
            }

            // Dead-ends (degree 1) and orphans (degree 0)
            m_valDeadEndNodes.clear();
            m_valOrphanNodes.clear();
            for (auto& [id, deg] : degree) {
                if (deg == 0) m_valOrphanNodes.push_back(id);
                else if (deg == 1) m_valDeadEndNodes.push_back(id);
            }
            m_valDeadEnds = static_cast<int>(m_valDeadEndNodes.size());
            m_valOrphans = static_cast<int>(m_valOrphanNodes.size());

            // Connected components (BFS)
            std::unordered_map<uint32_t, std::vector<uint32_t>> adj;
            for (const auto& edge : graph.GetEdges()) {
                if (degree.count(edge.fromNode) && degree.count(edge.toNode)) {
                    adj[edge.fromNode].push_back(edge.toNode);
                    adj[edge.toNode].push_back(edge.fromNode);
                }
            }
            std::unordered_set<uint32_t> visited;
            m_valComponents = 0;
            for (auto& [id, deg] : degree) {
                if (visited.count(id)) continue;
                ++m_valComponents;
                std::vector<uint32_t> queue = {id};
                visited.insert(id);
                while (!queue.empty()) {
                    uint32_t cur = queue.back(); queue.pop_back();
                    for (uint32_t nb : adj[cur]) {
                        if (!visited.count(nb)) {
                            visited.insert(nb);
                            queue.push_back(nb);
                        }
                    }
                }
            }

            // Duplicate edges
            m_valDuplicateEdges.clear();
            for (size_t i = 0; i < graph.GetEdges().size(); ++i) {
                const auto& ei = graph.GetEdges()[i];
                if (!degree.count(ei.fromNode) || !degree.count(ei.toNode)) continue;
                for (size_t j = 0; j < i; ++j) {
                    const auto& ej = graph.GetEdges()[j];
                    if ((ei.fromNode == ej.fromNode && ei.toNode == ej.toNode) ||
                        (ei.fromNode == ej.toNode && ei.toNode == ej.fromNode)) {
                        m_valDuplicateEdges.push_back(i);
                        break;
                    }
                }
            }
            m_valDuplicates = static_cast<int>(m_valDuplicateEdges.size());

            // Zero-length edges
            m_valZeroLength = 0;
            for (const auto& edge : graph.GetEdges()) {
                auto* from = graph.GetNode(edge.fromNode);
                auto* to = graph.GetNode(edge.toNode);
                if (!from || !to) continue;
                if (!degree.count(edge.fromNode) || !degree.count(edge.toNode)) continue;
                float dx = from->x - to->x;
                float dy = from->y - to->y;
                if (std::sqrt(dx * dx + dy * dy) < 0.1f)
                    ++m_valZeroLength;
            }

            m_showValidation = true;
        }

        ImGui::Separator();
        if (ImGui::MenuItem("Select All")) {
            selection.Clear();
            for (const auto& node : graph.GetNodes()) {
                if (node.mapId == mapId)
                    selection.SelectNode(node.id);
            }
            AutoSelectEdges(selection, graph);
        }
        if (ImGui::MenuItem("Deselect All")) {
            selection.Clear();
        }

        ImGui::EndPopup();
    }

    // === Validation results popup ===
    if (m_showValidation)
        ImGui::OpenPopup("Graph Validation");
    m_showValidation = false;

    if (ImGui::BeginPopup("Graph Validation")) {
        handled = true;
        ImGui::SeparatorText("Graph Validation Results");
        ImGui::Text("Connected components: %d%s", m_valComponents,
                     m_valComponents > 1 ? " (expected 1)" : "");
        ImGui::Text("Dead-end nodes: %d", m_valDeadEnds);
        ImGui::Text("Orphan nodes: %d", m_valOrphans);
        ImGui::Text("Duplicate edges: %d", m_valDuplicates);
        ImGui::Text("Zero-length edges: %d", m_valZeroLength);

        ImGui::Separator();
        if (m_valDeadEnds > 0 && ImGui::Button("Select Dead-ends")) {
            selection.Clear();
            for (uint32_t id : m_valDeadEndNodes)
                selection.SelectNode(id);
            ImGui::CloseCurrentPopup();
        }
        if (m_valOrphans > 0) {
            ImGui::SameLine();
            if (ImGui::Button("Select Orphans")) {
                selection.Clear();
                for (uint32_t id : m_valOrphanNodes)
                    selection.SelectNode(id);
                ImGui::CloseCurrentPopup();
            }
        }
        if (m_valDuplicates > 0) {
            ImGui::SameLine();
            if (ImGui::Button("Remove Duplicates")) {
                undo.Snapshot("Remove Duplicate Edges");
                std::vector<size_t> sorted = m_valDuplicateEdges;
                std::sort(sorted.begin(), sorted.end(), std::greater<size_t>());
                for (size_t idx : sorted)
                    graph.RemoveEdge(idx);
                ImGui::CloseCurrentPopup();
            }
        }

        if (ImGui::Button("Close"))
            ImGui::CloseCurrentPopup();

        ImGui::EndPopup();
    }

    ImGui::End(); // End ##GraphPopupHost

    return handled;
}

bool GraphEditor::ProcessInput(const Canvas& canvas, WorldGraphData& graph,
                                MultiSelection& selection, uint32_t mapId,
                                UndoContext& undo) {
    // === Render popups FIRST (must run every frame, even when WantCaptureMouse) ===
    bool popupHandled = RenderPopups(canvas, graph, selection, mapId, undo);

    if (ImGui::GetIO().WantCaptureMouse)
        return popupHandled;

    ImVec2 mouse = ImGui::GetIO().MousePos;
    float mx = mouse.x, my = mouse.y;

    // Check if in viewport
    if (mx < canvas.vpX || mx > canvas.vpX + canvas.vpW ||
        my < canvas.vpY || my > canvas.vpY + canvas.vpH)
        return false;

    bool shiftDown = ImGui::GetIO().KeyShift;
    bool ctrlDown  = ImGui::GetIO().KeyCtrl;

    // === Toggle edge mode with E key ===
    if (ImGui::IsKeyPressed(ImGuiKey_E) && !ImGui::GetIO().WantTextInput) {
        m_edgeMode = !m_edgeMode;
        m_edgeStartNode = 0;
        if (m_edgeMode) { m_drawMode = false; m_drawLastNode = 0; }
    }

    // === Toggle draw mode with D key ===
    if (ImGui::IsKeyPressed(ImGuiKey_D) && !ImGui::GetIO().WantTextInput) {
        m_drawMode = !m_drawMode;
        m_drawLastNode = 0;
        if (m_drawMode) { m_edgeMode = false; m_edgeStartNode = 0; }
    }

    // === Keyboard shortcuts ===
    if (!ImGui::GetIO().WantTextInput) {
        // S: split nearest edge at cursor
        if (ImGui::IsKeyPressed(ImGuiKey_S) && !ctrlDown) {
            int hitEdge = HitTestEdge(canvas, graph, mapId, mx, my);
            if (hitEdge >= 0) {
                size_t idx = static_cast<size_t>(hitEdge);
                const auto& edge = graph.GetEdges()[idx];
                auto* from = graph.GetNode(edge.fromNode);
                auto* to   = graph.GetNode(edge.toNode);
                if (from && to) {
                    undo.Snapshot("Split Edge");
                    float wx, wy;
                    canvas.ScreenToWorld(mx, my, wx, wy);

                    WorldNode mid;
                    mid.mapId = from->mapId;
                    mid.x = wx;
                    mid.y = wy;
                    mid.z = (from->z + to->z) * 0.5f;
                    mid.name = "";
                    mid.type = NodeType::Waypoint;

                    EdgeType etype = edge.type;
                    bool bidir = edge.bidirectional;
                    uint32_t fromId = edge.fromNode;
                    uint32_t toId = edge.toNode;

                    uint32_t midId = graph.AddNode(mid);
                    graph.RemoveEdge(idx);

                    auto calcCost = [&](uint32_t a, uint32_t b) -> float {
                        auto* na = graph.GetNode(a);
                        auto* nb = graph.GetNode(b);
                        if (!na || !nb) return 0;
                        float dx = na->x - nb->x;
                        float dy = na->y - nb->y;
                        return std::sqrt(dx * dx + dy * dy) / 7.0f;
                    };

                    WorldEdge e1;
                    e1.fromNode = fromId; e1.toNode = midId;
                    e1.type = etype; e1.bidirectional = bidir;
                    e1.cost = calcCost(fromId, midId);
                    graph.AddEdge(e1);

                    WorldEdge e2;
                    e2.fromNode = midId; e2.toNode = toId;
                    e2.type = etype; e2.bidirectional = bidir;
                    e2.cost = calcCost(midId, toId);
                    graph.AddEdge(e2);

                    selection.SetSingleNode(midId);
                    return true;
                }
            }
        }

        // Ctrl+A: select all nodes on current map
        if (ctrlDown && ImGui::IsKeyPressed(ImGuiKey_A)) {
            selection.Clear();
            for (const auto& node : graph.GetNodes()) {
                if (node.mapId == mapId)
                    selection.SelectNode(node.id);
            }
            AutoSelectEdges(selection, graph);
            return true;
        }

        // Escape: clear selection
        if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            if (!selection.Empty()) {
                selection.Clear();
                return true;
            }
        }

        // Delete: delete all selected
        if (ImGui::IsKeyPressed(ImGuiKey_Delete)) {
            if (!selection.Empty()) {
                undo.Snapshot("Delete Selection");
                // Delete edges first (sort descending)
                std::vector<size_t> edgeList(selection.edges.begin(), selection.edges.end());
                std::sort(edgeList.begin(), edgeList.end(), std::greater<size_t>());
                for (size_t idx : edgeList)
                    graph.RemoveEdge(idx);
                // Delete nodes
                std::vector<uint32_t> nodeList(selection.nodes.begin(), selection.nodes.end());
                for (uint32_t id : nodeList)
                    graph.RemoveNode(id);
                selection.Clear();
                return true;
            }
        }
    }

    // === Multi-node dragging ===
    if (m_drag.active) {
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            if (!m_drag.snapshotTaken) {
                ImVec2 delta = ImGui::GetMouseDragDelta(ImGuiMouseButton_Left, 2.0f);
                if (delta.x != 0.0f || delta.y != 0.0f) {
                    undo.Snapshot("Move Nodes");
                    m_drag.snapshotTaken = true;
                    // Capture start positions
                    float wx, wy;
                    canvas.ScreenToWorld(mx, my, wx, wy);
                    m_drag.anchorWx = wx;
                    m_drag.anchorWy = wy;
                    m_drag.startPositions.clear();
                    for (uint32_t id : selection.nodes) {
                        auto* node = graph.GetNode(id);
                        if (node)
                            m_drag.startPositions[id] = {node->x, node->y};
                    }
                }
            }
            if (m_drag.snapshotTaken) {
                float wx, wy;
                canvas.ScreenToWorld(mx, my, wx, wy);
                float deltaWx = wx - m_drag.anchorWx;
                float deltaWy = wy - m_drag.anchorWy;

                for (auto& [id, startPos] : m_drag.startPositions) {
                    auto* node = graph.GetNode(id);
                    if (node) {
                        node->x = startPos.first + deltaWx;
                        node->y = startPos.second + deltaWy;
                    }
                }
                graph.MarkDirty();

                // === Snap-to-nearest indicator ===
                if (ctrlDown && selection.nodes.size() == 1) {
                    uint32_t draggedId = *selection.nodes.begin();
                    auto* draggedNode = graph.GetNode(draggedId);
                    if (draggedNode) {
                        float dsx, dsy;
                        canvas.WorldToScreen(draggedNode->x, draggedNode->y, dsx, dsy);
                        float bestSnapDist = 15.0f;
                        const WorldNode* snapTarget = nullptr;
                        for (const auto& other : graph.GetNodes()) {
                            if (other.id == draggedId || other.mapId != mapId) continue;
                            if (selection.IsNodeSelected(other.id)) continue;
                            float osx, osy;
                            canvas.WorldToScreen(other.x, other.y, osx, osy);
                            float d = std::sqrt((osx - dsx) * (osx - dsx) + (osy - dsy) * (osy - dsy));
                            if (d < bestSnapDist) {
                                bestSnapDist = d;
                                snapTarget = &other;
                            }
                        }
                        if (snapTarget) {
                            float tsx, tsy;
                            canvas.WorldToScreen(snapTarget->x, snapTarget->y, tsx, tsy);
                            auto* dl = ImGui::GetForegroundDrawList();
                            dl->AddLine(ImVec2(dsx, dsy), ImVec2(tsx, tsy),
                                        IM_COL32(255, 255, 0, 180), 1.0f);
                            dl->AddCircle(ImVec2(tsx, tsy), 8.0f, IM_COL32(255, 255, 0, 200), 0, 2.0f);
                            // Snap position on Ctrl
                            draggedNode->x = snapTarget->x;
                            draggedNode->y = snapTarget->y;
                        }
                    }
                }
            }
            return true;
        } else {
            m_drag.active = false;
            m_drag.startPositions.clear();
        }
    }

    // === Lasso selection (Alt+drag) ===
    if (m_lassoSelecting) {
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            m_lassoPoints.push_back(ImVec2(mx, my));
            // Draw lasso polygon
            auto* dl = ImGui::GetForegroundDrawList();
            if (m_lassoPoints.size() >= 2) {
                for (size_t i = 0; i + 1 < m_lassoPoints.size(); ++i)
                    dl->AddLine(m_lassoPoints[i], m_lassoPoints[i + 1],
                                IM_COL32(255, 200, 100, 200), 1.5f);
                dl->AddLine(m_lassoPoints.back(), m_lassoPoints.front(),
                            IM_COL32(255, 200, 100, 100), 1.0f);
            }
            return true;
        } else {
            // Mouse released: point-in-polygon test
            if (!shiftDown)
                selection.Clear();

            auto pointInPoly = [](const std::vector<ImVec2>& poly, float px, float py) -> bool {
                bool inside = false;
                for (size_t i = 0, j = poly.size() - 1; i < poly.size(); j = i++) {
                    float xi = poly[i].x, yi = poly[i].y;
                    float xj = poly[j].x, yj = poly[j].y;
                    if (((yi > py) != (yj > py)) &&
                        (px < (xj - xi) * (py - yi) / (yj - yi) + xi))
                        inside = !inside;
                }
                return inside;
            };

            for (const auto& node : graph.GetNodes()) {
                if (node.mapId != mapId) continue;
                float nsx, nsy;
                canvas.WorldToScreen(node.x, node.y, nsx, nsy);
                if (pointInPoly(m_lassoPoints, nsx, nsy))
                    selection.SelectNode(node.id);
            }
            AutoSelectEdges(selection, graph);

            m_lassoSelecting = false;
            m_lassoPoints.clear();
            return true;
        }
    }

    // === Box selection ===
    if (m_boxSelecting) {
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            // Draw selection rectangle
            auto* dl = ImGui::GetForegroundDrawList();
            dl->AddRect(ImVec2(m_boxStartX, m_boxStartY), ImVec2(mx, my),
                        IM_COL32(100, 180, 255, 200), 0.0f, 0, 1.0f);
            dl->AddRectFilled(ImVec2(m_boxStartX, m_boxStartY), ImVec2(mx, my),
                              IM_COL32(100, 180, 255, 30));
            return true;
        } else {
            // Mouse released: compute selection
            float bx0 = (std::min)(m_boxStartX, mx);
            float by0 = (std::min)(m_boxStartY, my);
            float bx1 = (std::max)(m_boxStartX, mx);
            float by1 = (std::max)(m_boxStartY, my);

            if (!shiftDown)
                selection.Clear();

            for (const auto& node : graph.GetNodes()) {
                if (node.mapId != mapId) continue;
                float nsx, nsy;
                canvas.WorldToScreen(node.x, node.y, nsx, nsy);
                if (nsx >= bx0 && nsx <= bx1 && nsy >= by0 && nsy <= by1)
                    selection.SelectNode(node.id);
            }
            AutoSelectEdges(selection, graph);

            m_boxSelecting = false;
            return true;
        }
    }

    // === Draw mode ===
    if (m_drawMode) {
        // Visual: line from last node to cursor
        if (m_drawLastNode != 0) {
            auto* lastNode = graph.GetNode(m_drawLastNode);
            if (lastNode) {
                float lsx, lsy;
                canvas.WorldToScreen(lastNode->x, lastNode->y, lsx, lsy);
                auto* dl = ImGui::GetForegroundDrawList();
                dl->AddLine(ImVec2(lsx, lsy), ImVec2(mx, my),
                            IM_COL32(100, 255, 100, 150), 1.5f);
            }
        }
        // Cursor crosshair
        {
            auto* dl = ImGui::GetForegroundDrawList();
            dl->AddCircle(ImVec2(mx, my), 6.0f, IM_COL32(100, 255, 100, 200), 0, 1.5f);
        }

        // Left click: place node (or click existing to continue from it)
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            uint32_t hitNode = HitTestNode(canvas, graph, mapId, mx, my);

            if (hitNode) {
                // Clicked existing node: connect from chain if active, then continue from it
                if (m_drawLastNode != 0 && m_drawLastNode != hitNode) {
                    // Check no duplicate edge
                    bool duplicate = false;
                    for (const auto& edge : graph.GetEdges()) {
                        if ((edge.fromNode == m_drawLastNode && edge.toNode == hitNode) ||
                            (edge.fromNode == hitNode && edge.toNode == m_drawLastNode)) {
                            duplicate = true; break;
                        }
                    }
                    if (!duplicate) {
                        undo.Snapshot("Draw Connect");
                        auto* from = graph.GetNode(m_drawLastNode);
                        auto* to = graph.GetNode(hitNode);
                        if (from && to) {
                            WorldEdge edge;
                            edge.fromNode = m_drawLastNode;
                            edge.toNode = hitNode;
                            edge.type = EdgeType::Walk;
                            edge.bidirectional = true;
                            float dx = from->x - to->x;
                            float dy = from->y - to->y;
                            edge.cost = std::sqrt(dx * dx + dy * dy) / 7.0f;
                            graph.AddEdge(edge);
                        }
                    }
                }
                m_drawLastNode = hitNode;
                selection.SetSingleNode(hitNode);
            } else {
                float wx, wy;
                canvas.ScreenToWorld(mx, my, wx, wy);

                // If chain is active and click lands on an edge, auto-split and connect
                int hitEdge = (m_drawLastNode != 0) ? HitTestEdge(canvas, graph, mapId, mx, my) : -1;
                if (hitEdge >= 0) {
                    size_t idx = static_cast<size_t>(hitEdge);
                    const auto& edge = graph.GetEdges()[idx];
                    auto* efrom = graph.GetNode(edge.fromNode);
                    auto* eto   = graph.GetNode(edge.toNode);
                    if (efrom && eto) {
                        undo.Snapshot("Draw Split");

                        // Create split node at cursor position
                        WorldNode mid;
                        mid.mapId = efrom->mapId;
                        mid.x = wx;
                        mid.y = wy;
                        mid.z = (efrom->z + eto->z) * 0.5f;
                        mid.name = "";
                        mid.type = NodeType::Waypoint;

                        EdgeType etype = edge.type;
                        bool bidir = edge.bidirectional;
                        uint32_t fromId = edge.fromNode;
                        uint32_t toId = edge.toNode;

                        uint32_t midId = graph.AddNode(mid);
                        graph.RemoveEdge(idx);

                        auto calcCost = [&](uint32_t a, uint32_t b) -> float {
                            auto* na = graph.GetNode(a);
                            auto* nb = graph.GetNode(b);
                            if (!na || !nb) return 0;
                            float ddx = na->x - nb->x;
                            float ddy = na->y - nb->y;
                            return std::sqrt(ddx * ddx + ddy * ddy) / 7.0f;
                        };

                        // Reconstruct the split edge
                        WorldEdge e1;
                        e1.fromNode = fromId; e1.toNode = midId;
                        e1.type = etype; e1.bidirectional = bidir;
                        e1.cost = calcCost(fromId, midId);
                        graph.AddEdge(e1);

                        WorldEdge e2;
                        e2.fromNode = midId; e2.toNode = toId;
                        e2.type = etype; e2.bidirectional = bidir;
                        e2.cost = calcCost(midId, toId);
                        graph.AddEdge(e2);

                        // Connect chain to the new split node
                        bool duplicate = false;
                        for (const auto& ed : graph.GetEdges()) {
                            if ((ed.fromNode == m_drawLastNode && ed.toNode == midId) ||
                                (ed.fromNode == midId && ed.toNode == m_drawLastNode)) {
                                duplicate = true; break;
                            }
                        }
                        if (!duplicate) {
                            WorldEdge ce;
                            ce.fromNode = m_drawLastNode;
                            ce.toNode = midId;
                            ce.type = EdgeType::Walk;
                            ce.bidirectional = true;
                            ce.cost = calcCost(m_drawLastNode, midId);
                            graph.AddEdge(ce);
                        }

                        m_drawLastNode = midId;
                        selection.SetSingleNode(midId);
                    }
                } else {
                    // Create new node at cursor
                    undo.Snapshot("Draw Node");

                    WorldNode node;
                    node.mapId = mapId;
                    node.x = wx;
                    node.y = wy;
                    node.z = 0;
                    node.name = "";
                    node.type = NodeType::Waypoint;
                    uint32_t newId = graph.AddNode(node);

                    // Auto-connect to previous node
                    if (m_drawLastNode != 0) {
                        auto* from = graph.GetNode(m_drawLastNode);
                        auto* to = graph.GetNode(newId);
                        if (from && to) {
                            WorldEdge edge;
                            edge.fromNode = m_drawLastNode;
                            edge.toNode = newId;
                            edge.type = EdgeType::Walk;
                            edge.bidirectional = true;
                            float dx = from->x - to->x;
                            float dy = from->y - to->y;
                            edge.cost = std::sqrt(dx * dx + dy * dy) / 7.0f;
                            graph.AddEdge(edge);
                        }
                    }

                    m_drawLastNode = newId;
                    selection.SetSingleNode(newId);
                }
            }
            return true;
        }

        // Right click: delete node under cursor (or break chain)
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
            uint32_t hitNode = HitTestNode(canvas, graph, mapId, mx, my);
            if (hitNode) {
                undo.Snapshot("Delete Node");
                if (m_drawLastNode == hitNode)
                    m_drawLastNode = 0;
                graph.RemoveNode(hitNode);
                selection.Clear();
            } else {
                // Right-click on empty space: reset chain anchor
                m_drawLastNode = 0;
            }
            return true;
        }

        return false;
    }

    // === Right-click: context menu ===
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
        m_contextHitNode = HitTestNode(canvas, graph, mapId, mx, my);
        m_contextHitEdge = HitTestEdge(canvas, graph, mapId, mx, my);
        canvas.ScreenToWorld(mx, my, m_contextMenuWx, m_contextMenuWy);
        m_wantOpenContextMenu = true;
    }

    // === Left click ===
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

                    auto* from = graph.GetNode(m_edgeStartNode);
                    auto* to = graph.GetNode(hitNode);
                    if (from && to) {
                        float dx = from->x - to->x;
                        float dy = from->y - to->y;
                        edge.cost = std::sqrt(dx * dx + dy * dy) / 7.0f;
                    }

                    graph.AddEdge(edge);
                    m_edgeStartNode = 0;
                }
            }
            return true;
        }

        if (hitNode) {
            if (shiftDown) {
                selection.ToggleNode(hitNode);
            } else {
                if (!selection.IsNodeSelected(hitNode))
                    selection.SetSingleNode(hitNode);
                // Begin drag prep
                m_drag.active = true;
                m_drag.snapshotTaken = false;
                m_drag.startPositions.clear();
            }
            return true;
        }

        // Try edge hit
        int hitEdge = HitTestEdge(canvas, graph, mapId, mx, my);
        if (hitEdge >= 0) {
            if (shiftDown) {
                selection.ToggleEdge(static_cast<size_t>(hitEdge));
            } else {
                selection.SetSingleEdge(static_cast<size_t>(hitEdge));
            }
            return true;
        }

        // Click on empty space: start box or lasso selection (not in edge mode)
        if (!m_edgeMode) {
            bool altDown = ImGui::GetIO().KeyAlt;
            if (altDown) {
                m_lassoSelecting = true;
                m_lassoPoints.clear();
                m_lassoPoints.push_back(ImVec2(mx, my));
            } else {
                m_boxSelecting = true;
                m_boxStartX = mx;
                m_boxStartY = my;
            }
            if (!shiftDown)
                selection.Clear();
        } else {
            selection.Clear();
        }
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
        node.name = "";
        node.type = NodeType::Waypoint;

        uint32_t id = graph.AddNode(node);
        selection.SetSingleNode(id);
        m_boxSelecting = false; // cancel box select on double-click
        return true;
    }

    return false;
}

} // namespace mapedit
