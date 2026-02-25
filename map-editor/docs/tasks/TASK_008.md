# TASK-008: Interactive Road Graph Editor

**Status**: DONE
**Created**: 2026-02-25
**Depends on**: TASK-006 (road graph overlay, DONE)

---

## Overview

Transform the read-only road graph overlay (TASK-006) into a fully interactive graph editor with multi-selection, context menus, advanced graph operations (split edge, merge nodes, connect branches), bulk editing, and graph validation. The existing `GraphEditor` supports single-node editing for the world graph — this task extends it with professional-grade editing tools applicable to both world graph and road graph.

**Goal**: Give the user full control over the road graph — select, move, delete, connect, split, merge, straighten, validate — with undo/redo for every operation.

---

## Phase Status

| Phase | Description | Status |
|-------|-------------|--------|
| 1 | Multi-Selection (box select, shift-click, visual feedback) | DONE |
| 2 | Context Menu + Core Operations (delete, connect, split edge) | DONE |
| 3 | Drag Multi-Selection + Snap-to-Nearest | DONE |
| 4 | Merge Nodes + Straighten Path + Bulk Property Edit | DONE |
| 5 | Road Graph Edit Mode + Auto-Connect + Validate + Lasso | DONE |

---

## Phase 1: Multi-Selection Foundation

**Goal**: Replace the single-item `Selection` struct with a multi-selection system supporting box selection and shift-click.

### 1.1 Extend Selection Model

Current `selection.h` supports exactly one node OR one edge:
```cpp
// CURRENT — single selection
struct Selection {
    SelectionType type = SelectionType::None;
    uint32_t nodeId = 0;
    size_t edgeIndex = 0;
};
```

Replace with multi-selection:
```cpp
// NEW — multi-selection
#include <unordered_set>

struct MultiSelection {
    std::unordered_set<uint32_t> nodes;   // selected node IDs
    std::unordered_set<size_t>   edges;   // selected edge indices

    // --- Queries ---
    bool Empty() const { return nodes.empty() && edges.empty(); }
    bool HasNodes() const { return !nodes.empty(); }
    bool HasEdges() const { return !edges.empty(); }
    bool IsNodeSelected(uint32_t id) const { return nodes.count(id) > 0; }
    bool IsEdgeSelected(size_t idx) const { return edges.count(idx) > 0; }
    size_t Count() const { return nodes.size() + edges.size(); }

    // Single-selection convenience (backwards compat for property panel)
    bool IsSingleNode() const { return nodes.size() == 1 && edges.empty(); }
    bool IsSingleEdge() const { return edges.size() == 1 && nodes.empty(); }
    uint32_t SingleNodeId() const { return IsSingleNode() ? *nodes.begin() : 0; }
    size_t SingleEdgeIndex() const { return IsSingleEdge() ? *edges.begin() : 0; }

    // --- Mutations ---
    void Clear() { nodes.clear(); edges.clear(); }
    void SelectNode(uint32_t id) { nodes.insert(id); }
    void DeselectNode(uint32_t id) { nodes.erase(id); }
    void ToggleNode(uint32_t id) {
        if (nodes.count(id)) nodes.erase(id); else nodes.insert(id);
    }
    void SelectEdge(size_t idx) { edges.insert(idx); }
    void DeselectEdge(size_t idx) { edges.erase(idx); }
    void ToggleEdge(size_t idx) {
        if (edges.count(idx)) edges.erase(idx); else edges.insert(idx);
    }
    void SetSingleNode(uint32_t id) { Clear(); nodes.insert(id); }
    void SetSingleEdge(size_t idx) { Clear(); edges.insert(idx); }

    // Select all edges connecting two selected nodes (auto-select)
    void SelectConnectingEdges(const WorldGraphData& graph);
};
```

**Migration**: Replace `Selection` with `MultiSelection` everywhere. `IsSingleNode()`/`IsSingleEdge()` provide backwards compatibility so property panel and renderer work without rewriting.

### 1.2 Box Selection (Drag Rectangle)

**Trigger**: Left-click on empty space + drag (when NOT in edge mode, NOT clicking a node/edge).

**State in GraphEditor**:
```cpp
bool  m_boxSelecting = false;
float m_boxStartX = 0, m_boxStartY = 0;   // screen coords
```

**Logic**:
1. `IsMouseClicked(Left)` on empty space (no hit test match) → start box select: save `m_boxStartX/Y = mouse`, set `m_boxSelecting = true`
2. While `IsMouseDown(Left)` and `m_boxSelecting` → draw selection rectangle on foreground draw list
3. On `IsMouseReleased(Left)` → compute world-space AABB from box corners, find all nodes within AABB:
   ```cpp
   float wx1, wy1, wx2, wy2;
   canvas.ScreenToWorld(m_boxStartX, m_boxStartY, wx1, wy1);
   canvas.ScreenToWorld(mx, my, wx2, wy2);
   float minWx = std::min(wx1, wx2), maxWx = std::max(wx1, wx2);
   float minWy = std::min(wy1, wy2), maxWy = std::max(wy1, wy2);

   for (auto& node : graph.GetNodes()) {
       if (node.mapId != mapId) continue;
       if (node.x >= minWx && node.x <= maxWx &&
           node.y >= minWy && node.y <= maxWy) {
           selection.SelectNode(node.id);
       }
   }
   ```
4. If `Shift` held — ADD to existing selection. If not — REPLACE selection.
5. Optionally auto-select edges whose BOTH endpoints are selected.

**Visual feedback**: Semi-transparent blue rectangle with 1px white border:
```cpp
if (m_boxSelecting) {
    auto* dl = ImGui::GetForegroundDrawList();
    ImVec2 p1(m_boxStartX, m_boxStartY), p2(mx, my);
    dl->AddRectFilled(p1, p2, IM_COL32(50, 100, 200, 40));
    dl->AddRect(p1, p2, IM_COL32(200, 200, 255, 200), 0, 0, 1.0f);
}
```

### 1.3 Shift+Click Multi-Select

**Logic in ProcessInput**:
- `IsMouseClicked(Left)` + hit test finds node/edge:
  - If `Shift` held → `selection.ToggleNode(hitNode)` (add or remove from selection)
  - If `Shift` NOT held → `selection.SetSingleNode(hitNode)` (replace selection)
- Same logic for edge hits.

### 1.4 Keyboard Shortcuts

- **Ctrl+A**: Select all nodes on current map
- **Escape**: Clear selection
- **Delete**: Delete all selected nodes + edges (Phase 2)

### 1.5 Rendering Updates

**graph_renderer.cpp** — update `Render()` to check multi-selection:
```cpp
// Node highlight
bool selected = selection.IsNodeSelected(node.id);

// Edge highlight
bool selected = selection.IsEdgeSelected(i);
```

**Road overlay** — when road graph is in edit mode (Phase 5), pass selection to `RenderRoadOverlay` too.

### 1.6 Property Panel Updates

**property_panel.cpp** — handle multi-selection:
- `IsSingleNode()` → show full node editor (existing behavior)
- `IsSingleEdge()` → show full edge editor (existing behavior)
- `Count() > 1` → show summary: "N nodes, M edges selected"
- Bulk operations go here (Phase 4)

### 1.7 Files to Modify

| File | Changes |
|------|---------|
| `src/editor/selection.h` | Replace `Selection` with `MultiSelection` struct |
| `src/editor/graph_editor.h` | Add box select state (`m_boxSelecting`, `m_boxStartX/Y`) |
| `src/editor/graph_editor.cpp` | Box select logic, shift-click, Ctrl+A, Escape |
| `src/render/graph_renderer.cpp` | Multi-selection highlight in `Render()` |
| `src/ui/property_panel.h` | Update signature: `Selection&` → `MultiSelection&` |
| `src/ui/property_panel.cpp` | Multi-selection summary display |
| `src/app.h` | `Selection m_selection` → `MultiSelection m_selection` |
| `src/app.cpp` | Update all `m_selection` usage sites |
| `src/editor/route_editor.cpp` | Adapt to new selection type (route editor doesn't use graph selection, but verify no conflicts) |

### 1.8 Verification

- [ ] Click node → single-select (yellow highlight), other deselected
- [ ] Shift+click node → adds to selection (multiple yellow highlights)
- [ ] Shift+click selected node → deselects it
- [ ] Drag rectangle on empty space → all nodes inside highlighted
- [ ] Shift+drag rectangle → adds to existing selection
- [ ] Ctrl+A → all nodes on map selected
- [ ] Escape → all deselected
- [ ] Property panel shows "N nodes, M edges selected" for multi-select
- [ ] Property panel shows single-node/edge editor for single selection (backwards compat)
- [ ] Undo/redo still works for single-node operations

---

## Phase 2: Context Menu + Core Operations

**Goal**: Right-click context menu with delete, connect, and split-edge operations.

### 2.1 Context Menu System

**Trigger**: Right-click (`ImGuiMouseButton_Right`) on canvas.

**State in GraphEditor**:
```cpp
bool     m_contextMenuOpen = false;
float    m_contextMenuX = 0, m_contextMenuY = 0;   // screen coords where menu was opened
uint32_t m_contextHitNode = 0;                       // node under cursor (0=none)
int      m_contextHitEdge = -1;                      // edge under cursor (-1=none)
```

**Logic**:
1. On `IsMouseClicked(Right)`:
   - Run hit tests (`HitTestNode`, `HitTestEdge`)
   - If hit node is NOT in current selection — select it (replace or add depending on Shift)
   - Store hit results in `m_contextHitNode`/`m_contextHitEdge`
   - Call `ImGui::OpenPopup("GraphContextMenu")`

2. Render popup with `ImGui::BeginPopup("GraphContextMenu")`:
   ```
   Context menu structure:
   ┌────────────────────────────┐
   │ Delete (N nodes, M edges)  │  ← if selection non-empty
   │ ─────────────────────────  │
   │ Connect Nodes              │  ← if exactly 2 nodes selected
   │ Split Edge                 │  ← if edge under cursor or single edge selected
   │ ─────────────────────────  │
   │ Add Node Here              │  ← always available
   │ ─────────────────────────  │
   │ Select All                 │
   │ Deselect All               │
   └────────────────────────────┘
   ```

### 2.2 Delete Selected

**Action**: Remove all selected nodes and edges.

```cpp
void DeleteSelected(WorldGraphData& graph, MultiSelection& selection, UndoContext& undo) {
    if (selection.Empty()) return;
    undo.Snapshot("Delete Selection");

    // Delete edges first (indices shift when removing nodes)
    // Sort indices descending to avoid invalidation
    std::vector<size_t> edgeIndices(selection.edges.begin(), selection.edges.end());
    std::sort(edgeIndices.rbegin(), edgeIndices.rend());
    for (size_t idx : edgeIndices) {
        graph.RemoveEdge(idx);
    }

    // Delete nodes (RemoveNode also cascade-removes connected edges)
    for (uint32_t id : selection.nodes) {
        graph.RemoveNode(id);
    }

    selection.Clear();
}
```

**Important**: Edge indices become invalid as edges are removed. Sort descending and remove from back to front. Node removal via `RemoveNode()` already cascade-deletes connected edges.

### 2.3 Connect Nodes

**Precondition**: Exactly 2 nodes selected.

**Action**: Create edge between them (same as edge-mode but via context menu).

```cpp
void ConnectSelectedNodes(WorldGraphData& graph, MultiSelection& selection, UndoContext& undo) {
    if (selection.nodes.size() != 2) return;
    auto it = selection.nodes.begin();
    uint32_t id1 = *it++;
    uint32_t id2 = *it;

    // Check if edge already exists
    for (const auto& e : graph.GetEdges()) {
        if ((e.fromNode == id1 && e.toNode == id2) ||
            (e.fromNode == id2 && e.toNode == id1))
            return; // already connected
    }

    undo.Snapshot("Connect Nodes");
    WorldEdge edge;
    edge.fromNode = id1;
    edge.toNode = id2;
    edge.type = EdgeType::Walk;
    edge.bidirectional = true;

    auto* n1 = graph.GetNode(id1);
    auto* n2 = graph.GetNode(id2);
    if (n1 && n2) {
        float dx = n1->x - n2->x, dy = n1->y - n2->y;
        edge.cost = std::sqrt(dx * dx + dy * dy) / 7.0f;
    }
    graph.AddEdge(edge);
}
```

### 2.4 Split Edge (Insert Node on Edge)

**Precondition**: One edge selected (via context menu hit or single selection).

**Action**: Insert a new node at the midpoint of the edge, replace original edge with two edges.

```cpp
void SplitEdge(WorldGraphData& graph, MultiSelection& selection,
               size_t edgeIdx, UndoContext& undo) {
    if (edgeIdx >= graph.GetEdges().size()) return;
    const auto& edge = graph.GetEdges()[edgeIdx];

    auto* from = graph.GetNode(edge.fromNode);
    auto* to   = graph.GetNode(edge.toNode);
    if (!from || !to) return;

    undo.Snapshot("Split Edge");

    // Create midpoint node
    WorldNode mid;
    mid.mapId = from->mapId;
    mid.x = (from->x + to->x) * 0.5f;
    mid.y = (from->y + to->y) * 0.5f;
    mid.z = (from->z + to->z) * 0.5f;
    mid.name = "";
    mid.type = NodeType::Waypoint;
    uint32_t midId = graph.AddNode(mid);

    // Remember original edge properties
    EdgeType etype = edge.type;
    bool bidir = edge.bidirectional;
    uint32_t fromId = edge.fromNode;
    uint32_t toId = edge.toNode;

    // Remove original edge
    graph.RemoveEdge(edgeIdx);

    // Create two new edges: from→mid, mid→to
    auto makeHalf = [&](uint32_t a, uint32_t b) {
        WorldEdge e;
        e.fromNode = a;
        e.toNode = b;
        e.type = etype;
        e.bidirectional = bidir;
        auto* na = graph.GetNode(a);
        auto* nb = graph.GetNode(b);
        if (na && nb) {
            float dx = na->x - nb->x, dy = na->y - nb->y;
            e.cost = std::sqrt(dx * dx + dy * dy) / 7.0f;
        }
        graph.AddEdge(e);
    };
    makeHalf(fromId, midId);
    makeHalf(midId, toId);

    // Select new node so user can drag it
    selection.SetSingleNode(midId);
}
```

### 2.5 Add Node via Context Menu

**Action**: Same as existing double-click-to-add, but triggered from context menu at cursor position.

### 2.6 Files to Modify

| File | Changes |
|------|---------|
| `src/editor/graph_editor.h` | Context menu state fields |
| `src/editor/graph_editor.cpp` | Right-click handler, popup rendering, delete/connect/split logic |
| `src/data/world_graph_data.h` | (no changes — AddNode/RemoveNode/AddEdge/RemoveEdge already exist) |

### 2.7 Verification

- [ ] Right-click on node → context menu appears, node becomes selected
- [ ] Right-click on edge → context menu with "Split Edge" option
- [ ] Right-click on empty space → context menu with "Add Node Here"
- [ ] "Delete" removes all selected nodes + their edges, single Ctrl+Z restores all
- [ ] "Connect Nodes" (2 nodes selected) creates edge, Ctrl+Z removes it
- [ ] "Connect Nodes" disabled/hidden when !=2 nodes selected
- [ ] "Split Edge" inserts node at midpoint, creates 2 new edges, Ctrl+Z restores original edge
- [ ] New node from split is selected and immediately draggable
- [ ] Context menu doesn't appear when mouse is over ImGui UI panels

---

## Phase 3: Drag Multi-Selection + Snap-to-Nearest

**Goal**: Move a group of selected nodes together. Show snap indicator when dragging near an unselected node.

### 3.1 Multi-Node Drag

**Current**: `GraphEditor` drags a single node. Extend to drag all selected nodes.

**State**:
```cpp
struct DragState {
    bool     active = false;
    bool     snapshotTaken = false;
    float    anchorWx = 0, anchorWy = 0;   // world position at drag start (under cursor)
    std::unordered_map<uint32_t, std::pair<float,float>> startPositions; // nodeId → (x,y) at drag start
};
DragState m_drag;
```

**Logic**:
1. On left-click on a selected node → begin drag preparation (don't move yet)
2. When mouse moves >= 2px (existing threshold) → take undo snapshot "Move N Nodes", capture all selected node positions
3. Each frame: compute world delta from anchor, apply to all selected nodes:
   ```cpp
   float wx, wy;
   canvas.ScreenToWorld(mx, my, wx, wy);
   float deltaX = wx - m_drag.anchorWx;
   float deltaY = wy - m_drag.anchorWy;

   for (auto& [id, startPos] : m_drag.startPositions) {
       auto* node = graph.GetNode(id);
       if (node) {
           node->x = startPos.first + deltaX;
           node->y = startPos.second + deltaY;
       }
   }
   graph.MarkDirty();
   ```
4. On mouse release → finalize drag, clear state.

### 3.2 Snap-to-Nearest Indicator

**Purpose**: When dragging a node (single or group) near an unselected node, show a visual snap guide.

**Logic** (each frame during drag):
1. Find the dragged node closest to the cursor (for single-drag this is the dragged node itself)
2. Find the closest unselected node within `snapRadius` (e.g., 15px screen space)
3. If found — draw a dashed line from dragged node to snap target + highlight snap target

**Snap execution**: NOT automatic (user must explicitly connect via context menu or edge mode). The snap indicator is purely visual guidance.

**Optional enhancement**: Hold `Ctrl` during drag → snap the cursor node to the exact position of the nearest unselected node. This helps align branches.

### 3.3 Files to Modify

| File | Changes |
|------|---------|
| `src/editor/graph_editor.h` | `DragState m_drag` struct |
| `src/editor/graph_editor.cpp` | Multi-node drag logic, snap indicator rendering |

### 3.4 Verification

- [ ] Select 5 nodes → drag one → all 5 move together maintaining relative positions
- [ ] Single Ctrl+Z restores all 5 to original positions
- [ ] Drag near unselected node → snap indicator (dashed line) appears
- [ ] Snap indicator disappears when cursor moves away
- [ ] Ctrl+drag snaps dragged node position to nearest unselected node

---

## Phase 4: Merge Nodes + Straighten Path + Bulk Property Edit

**Goal**: Advanced graph topology operations.

### 4.1 Merge Nodes

**Precondition**: 2+ nodes selected.

**Action**: Merge all selected nodes into one. All edges connected to merged nodes reconnect to the survivor.

**Algorithm**:
```
1. Pick survivor = first selected node (or the one with the most connections)
2. Set survivor position = centroid of all selected nodes
3. For each non-survivor selected node:
   a. For each edge referencing this node:
      - Remap fromNode/toNode to survivor ID
      - Skip if this creates a self-loop (from==to)
      - Skip if this duplicates an existing edge
   b. Remove the node
4. Clean up selection: only survivor remains selected
```

**Context menu entry**: "Merge Nodes" (visible when 2+ nodes selected)

**Undo**: Single snapshot before merge → Ctrl+Z restores all original nodes + edges.

### 4.2 Straighten Path

**Precondition**: 3+ nodes selected that form a connected chain.

**Action**: Align intermediate nodes along the straight line between the two endpoints.

**Algorithm**:
```
1. Build adjacency from selected edges or from edges connecting selected nodes
2. Find the two endpoint nodes (degree 1 within selection subgraph)
3. If no clear endpoints (cycle) — use the two most distant nodes
4. Order nodes along the chain: BFS/DFS from endpoint A to endpoint B
5. Lerp intermediate nodes along the line A→B:
   for i in 1..N-2:
       t = i / (N-1)
       node[i].x = A.x + t * (B.x - A.x)
       node[i].y = A.y + t * (B.y - A.y)
6. Recalculate edge costs for affected edges
```

**Context menu entry**: "Straighten Path" (visible when 3+ connected nodes selected)

### 4.3 Bulk Property Edit

**Precondition**: 2+ nodes selected.

**Property panel behavior** in multi-select mode:
```
┌─────────────────────────┐
│ 12 nodes, 8 edges       │
│ ─────────────────────── │
│ Type: [Waypoint  ▼]     │  ← combo, sets ALL selected nodes
│ Faction: [________]     │  ← input, sets ALL selected nodes
│ ─────────────────────── │
│ [Delete All Selected]   │
│ [Merge Nodes]           │
│ [Straighten Path]       │
└─────────────────────────┘
```

When user changes Type or Faction → apply to ALL selected nodes (with undo snapshot).

For edge multi-select:
```
│ Edge Type: [Walk ▼]     │  ← sets ALL selected edges
│ Bidirectional: [☑]      │  ← sets ALL selected edges
│ [Recalc Costs]          │  ← recalculate cost from distance for all
```

### 4.4 Files to Modify

| File | Changes |
|------|---------|
| `src/editor/graph_editor.h` | Merge and straighten method declarations |
| `src/editor/graph_editor.cpp` | Merge, straighten algorithms |
| `src/ui/property_panel.cpp` | Bulk property editing UI |

### 4.5 Verification

- [ ] Select 3 nearby nodes → "Merge Nodes" → one node at centroid with all edges reconnected
- [ ] Ctrl+Z after merge → all 3 nodes restored with original edges
- [ ] No duplicate edges after merge, no self-loops
- [ ] Select chain of 5 nodes → "Straighten Path" → 3 intermediate nodes aligned on line
- [ ] Ctrl+Z after straighten → nodes return to original positions
- [ ] Select 10 nodes → change Type in property panel → all 10 change, Ctrl+Z reverts all
- [ ] "Recalc Costs" recalculates edge.cost = distance / 7.0 for all selected edges

---

## Phase 5: Road Graph Edit Mode + Auto-Connect + Validate + Lasso

**Goal**: Make road graph editable, add automation tools and validation.

### 5.1 Road Graph Edit Mode Toggle

**Current**: `m_roadGraphData` is read-only, always uses `RenderRoadOverlay()`.

**New**: Add edit mode switch so user can edit either world graph or road graph (not both simultaneously).

**State in App**:
```cpp
enum class ActiveGraph { World, Road };
ActiveGraph m_activeGraph = ActiveGraph::World;
```

**UI**: Toolbar toggle button or dropdown: `[World Graph ▼]` / `[Road Graph ▼]`

**When `m_activeGraph == Road`**:
- `GraphEditor::ProcessInput()` receives `m_roadGraphData` instead of `m_graphData`
- `GraphRenderer::Render()` renders road graph with full selection support
- `RenderRoadOverlay()` renders world graph as dim overlay (roles swap)
- Property panel shows road graph data
- Undo/redo works on road graph

**UndoRedo extension**: The current `EditorSnapshot` captures `(nodes, edges, routes)` — this stores the world graph state. For road graph editing, need to also capture road graph state. Options:

Option A (simple): Separate `UndoRedo` instance for road graph — `m_roadUndoRedo`. When active graph switches, undo/redo binds to the appropriate stack.

Option B (unified): Extend `EditorSnapshot` to store both graphs. More complex but single undo stack.

**Recommendation**: Option A — two separate `UndoRedo` instances. Cleaner separation, no risk of bloating snapshots.

```cpp
// app.h
UndoRedo m_undoRedo;       // for world graph
UndoRedo m_roadUndoRedo;   // for road graph
```

The active UndoContext switches based on `m_activeGraph`.

### 5.2 Save Road Graph

**Current**: Road graph has "Open" but no "Save". Add:

- Menu item **Road Graph > Save Road Graph** (to current path)
- Menu item **Road Graph > Save Road Graph As...** (file dialog)
- Dirty indicator in title bar for road graph

### 5.3 Auto-Connect Nearby Nodes

**Purpose**: After importing from Python pipeline, many branch endpoints are close but not connected. Auto-connect finds these and creates edges.

**Parameters** (shown in dialog or toolbar):
- `float maxDistance = 50.0f` — maximum world-distance to auto-connect (yards)

**Algorithm**:
```
1. For each node A on current map:
   2. For each node B where B.id > A.id (avoid duplicates):
      3. If distance(A, B) <= maxDistance AND no edge exists between A and B:
         4. Check if both are "endpoints" (degree <= 1 in current graph) — optional filter
         5. Create edge Walk, bidirectional, cost = distance / 7.0
```

**Context menu / toolbar entry**: "Auto-Connect (< N yards)" with distance input.

**Undo**: Single snapshot before all connections → one Ctrl+Z undoes all.

### 5.4 Graph Validation

**Purpose**: Detect structural issues in the graph.

**Checks**:
1. **Disconnected components**: Find connected components via BFS/DFS. Report if more than one component exists on the map.
2. **Dead-end nodes**: Nodes with degree 1 (only one edge). May be intentional (dungeon entrances) or accidental.
3. **Duplicate edges**: Two edges connecting the same pair of nodes.
4. **Zero-length edges**: Edge where from==to position (or extremely close, < 0.1 yards).
5. **Orphan nodes**: Nodes with degree 0 (no edges at all).

**UI**: "Validate Graph" button (toolbar or menu) → results in a popup/log window:
```
Graph Validation Results:
  Connected components: 3 (expected 1)
  Dead-end nodes: 12
  Duplicate edges: 2
  Zero-length edges: 0
  Orphan nodes: 5

  [Select Dead-ends] [Select Orphans] [Remove Duplicates]
```

Buttons auto-select the problematic elements so user can review/fix them.

### 5.5 Lasso Selection (Free-form)

**Trigger**: Hold `Alt` + left-click-drag → free-form selection polygon.

**State**:
```cpp
bool m_lassoActive = false;
std::vector<ImVec2> m_lassoPoints;   // screen coords
```

**Logic**:
1. `Alt + IsMouseClicked(Left)` → start lasso, clear points
2. While dragging → append mouse position to `m_lassoPoints` every N pixels (avoid excessive density)
3. On release → close polygon, test each node: point-in-polygon check
4. Select all nodes inside the polygon

**Visual**: Draw polyline of lasso points + fill with semi-transparent color:
```cpp
if (m_lassoActive && m_lassoPoints.size() >= 3) {
    auto* dl = ImGui::GetForegroundDrawList();
    dl->AddConvexPolyFilled(m_lassoPoints.data(), (int)m_lassoPoints.size(),
                            IM_COL32(50, 200, 100, 30));
    dl->AddPolyline(m_lassoPoints.data(), (int)m_lassoPoints.size(),
                    IM_COL32(100, 255, 150, 200), ImDrawFlags_Closed, 1.5f);
}
```

**Note**: `AddConvexPolyFilled` requires convex polygon. For arbitrary lasso shapes, use triangulation or fallback to per-point render. Simpler approach: use point-in-polygon test (ray casting) which works for any polygon, and render with `AddPolyline` only (no fill).

### 5.6 Files to Modify / Create

| File | Changes |
|------|---------|
| `src/app.h` | `ActiveGraph m_activeGraph`, `UndoRedo m_roadUndoRedo` |
| `src/app.cpp` | Active graph switching, undo context routing, save road graph, toolbar toggle |
| `src/ui/main_menu.h` | Save road graph actions |
| `src/ui/main_menu.cpp` | "Save Road Graph" / "Save Road Graph As..." menu items |
| `src/editor/graph_editor.h` | Lasso state, auto-connect, validate methods |
| `src/editor/graph_editor.cpp` | Lasso logic, auto-connect algorithm, validation checks |
| `src/render/graph_renderer.h` | Update `RenderRoadOverlay` to accept optional selection for edit mode |
| `src/render/graph_renderer.cpp` | Road overlay with selection support when in edit mode |

### 5.7 Verification

- [ ] Toggle to Road Graph mode → road graph becomes interactive (select, drag, delete)
- [ ] World graph dims to overlay when Road Graph is active
- [ ] Ctrl+Z in road graph mode undoes road graph edits (separate undo stack)
- [ ] Switch back to World Graph mode → road graph returns to read-only overlay
- [ ] Save Road Graph writes JSON, re-loads correctly
- [ ] Auto-Connect creates edges between close endpoint nodes
- [ ] Ctrl+Z after auto-connect removes all auto-created edges
- [ ] Validate Graph detects disconnected components and reports count
- [ ] "Select Dead-ends" highlights all degree-1 nodes
- [ ] "Select Orphans" highlights all degree-0 nodes
- [ ] "Remove Duplicates" removes duplicate edges (with undo)
- [ ] Alt+drag draws lasso, selects nodes inside on release
- [ ] Lasso respects Shift (add to selection)

---

## Implementation Order

```
Phase 1: Multi-Selection Foundation
    ↓
Phase 2: Context Menu + Core Operations
    ↓
Phase 3: Drag Multi-Selection + Snap
    ↓
Phase 4: Merge + Straighten + Bulk Edit
    ↓
Phase 5: Road Graph Edit Mode + Auto-Connect + Validate + Lasso
```

Each phase is independently testable and adds user-facing value. Phase 1 is the foundation — all subsequent phases depend on it. Phases 2-4 can theoretically be done in any order, but the listed order builds complexity gradually. Phase 5 ties everything together by making the road graph editable.

---

## Key Design Decisions

### Why `MultiSelection` replaces `Selection` entirely
Single-selection is just multi-selection with count==1. The `IsSingleNode()`/`IsSingleEdge()` helpers preserve backwards compatibility so existing code (property panel, renderers) works with minimal changes. No need to maintain two parallel selection systems.

### Why separate UndoRedo for road graph (Phase 5)
The current `EditorSnapshot` stores `(nodes, edges, routes)` from the world graph. Extending it to store both graphs doubles snapshot memory usage even when only one graph is being edited. Two separate stacks are cleaner: the active stack follows `m_activeGraph`.

### Why NOT auto-snap on drag
Auto-snapping (teleporting a node to exact position of nearest node) is surprising UX. Instead, we show a visual indicator and let the user explicitly merge/connect. The optional `Ctrl+drag` snap is opt-in.

### Why context menu over dedicated toolbar buttons
Context menu is faster for spatial operations (split edge at this location, delete this node). Toolbar buttons require click-select-then-click-button. The context menu respects current selection and hit-test context. Toolbar buttons are still used for global operations (validate, auto-connect, active graph toggle).

---

## Out of Scope

- 3D editing of road graph (road nodes have z=0)
- Pathfinding on road graph (road graph is visual reference, not a navigation mesh)
- Import from Python pipeline within the editor (user loads JSON via File menu)
- Multi-map road graph editing (one map at a time)
- Collaborative editing / multi-user
