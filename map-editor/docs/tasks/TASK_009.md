# TASK-009: 3D Road Graph Editing

**Status**: DONE
**Created**: 2026-02-26
**Depends on**: TASK-008 (Interactive Road Graph Editor, DONE)

---

## Overview

Port the full 2D Road Graph editing experience (TASK-008) into 3D mode. Currently, 3D mode only **visualizes** the road graph (via `Graph3DRenderer`) but provides zero interactive editing — all editing must be done in 2D. This task adds complete graph editing in 3D, mirroring every 2D feature except Z-axis manipulation via node dragging (Z is editable only through the Property Panel).

**Goal**: The user can switch to 3D mode and perform every editing operation available in 2D — select, move, delete, connect, split, merge, straighten, draw mode, edge mode, lasso/box select, auto-connect, validate — without switching back to 2D.

**Constraint**: Node Z coordinates are NOT modified by dragging in 3D. Dragging moves nodes only in the XY plane (horizontal). Z editing is available through the Property Panel as before.

---

## Phase Status

| Phase | Description | Status |
|-------|-------------|--------|
| 1 | 3D Hit Testing + Single/Multi Selection (click, shift-click, box select) | DONE |
| 2 | 3D Node Dragging (XY-plane constrained) + Snap-to-Nearest | DONE |
| 3 | 3D Context Menu + Core Operations (delete, connect, split, add node) | DONE |
| 4 | 3D Edge Mode + Draw Mode | DONE |
| 5 | 3D Lasso Selection + Keyboard Shortcuts + Full Parity Verification | DONE |

---

## Architecture Decision: Unified GraphEditor vs Separate Graph3DEditor

**Decision**: Extend `GraphEditor` to support both 2D and 3D modes via an **abstraction layer** for coordinate transforms and hit testing.

**Rationale**: All editing logic (delete, connect, split, merge, straighten, auto-connect, validate, undo/redo, selection management, draw mode state machine, edge mode state machine) is identical between 2D and 3D. The ONLY differences are:
1. **Screen-to-world** — 2D uses `Canvas::ScreenToWorld()`, 3D uses `Camera3D::ScreenToRay()` + plane intersection
2. **World-to-screen** — 2D uses `Canvas::WorldToScreen()`, 3D uses `Camera3D::WorldToScreen()`
3. **Hit testing** — same screen-space distance logic, different projection source
4. **Node dragging** — 2D uses direct `ScreenToWorld` delta, 3D uses ray-plane intersection delta
5. **Visual feedback** — both use `ImGui::GetForegroundDrawList()` for screen-space overlays (box select, lasso, snap indicator)

Duplicating the entire `GraphEditor` for 3D would create ~1100 lines of near-identical code. Instead, we introduce a small interface that abstracts the projection:

```cpp
// graph_editor.h

struct EditorProjection {
    // Project world position to screen coordinates
    // Returns false if the point is behind the camera (3D only)
    virtual bool WorldToScreen(float wx, float wy, float wz,
                               float& sx, float& sy) const = 0;

    // Convert screen position to world XY (Z is provided as reference height)
    // In 2D: ignores refZ, uses Canvas::ScreenToWorld
    // In 3D: casts ray via ScreenToRay, intersects horizontal plane at refZ
    virtual bool ScreenToWorldXY(float sx, float sy, float refZ,
                                 float& wx, float& wy) const = 0;

    virtual ~EditorProjection() = default;
};

// 2D implementation (wraps Canvas)
struct EditorProjection2D : EditorProjection {
    const Canvas* canvas = nullptr;

    bool WorldToScreen(float wx, float wy, float wz,
                       float& sx, float& sy) const override {
        canvas->WorldToScreen(wx, wy, sx, sy);  // 2D ignores Z
        return true;  // always visible in 2D
    }

    bool ScreenToWorldXY(float sx, float sy, float refZ,
                         float& wx, float& wy) const override {
        canvas->ScreenToWorld(sx, sy, wx, wy);
        return true;
    }
};

// 3D implementation (wraps Camera3D)
struct EditorProjection3D : EditorProjection {
    const Camera3D* camera = nullptr;

    bool WorldToScreen(float wx, float wy, float wz,
                       float& sx, float& sy) const override {
        return camera->WorldToScreen(wx, wy, wz, sx, sy);
    }

    bool ScreenToWorldXY(float sx, float sy, float refZ,
                         float& wx, float& wy) const override {
        float ox, oy, oz, dx, dy, dz;
        camera->ScreenToRay(sx, sy, ox, oy, oz, dx, dy, dz);

        // Intersect ray with horizontal plane Z = refZ
        if (fabsf(dz) < 0.0001f) return false;  // ray parallel to plane
        float t = (refZ - oz) / dz;
        if (t <= 0.0f) return false;  // behind camera

        wx = ox + dx * t;
        wy = oy + dy * t;
        return true;
    }
};
```

`GraphEditor::ProcessInput` currently takes `Canvas&` — we change it to take `EditorProjection&` (or add an overload). Internally, all hit tests and coordinate conversions go through this interface.

---

## Phase 1: 3D Hit Testing + Selection

**Goal**: Click on nodes/edges in 3D to select them. Box selection works. Shift+click and Ctrl+A work.

### 1.1 EditorProjection Abstraction

Create the `EditorProjection` interface and its two implementations as described above.

**Refactor `GraphEditor::ProcessInput` signature**:

Current:
```cpp
void ProcessInput(Canvas& canvas, WorldGraphData& graph, MultiSelection& selection,
                  uint32_t mapId, UndoContext& undo, const TerrainHeightSampler* sampler);
```

New:
```cpp
void ProcessInput(const EditorProjection& proj, WorldGraphData& graph,
                  MultiSelection& selection, uint32_t mapId, UndoContext& undo,
                  const TerrainHeightSampler* sampler);
```

All callers in `App::RenderFrame2D()` pass `EditorProjection2D{&m_canvas}`, and `App::RenderFrame3D()` passes `EditorProjection3D{&m_camera3d}`.

### 1.2 Refactor HitTestNode for Projection-Agnostic Use

Current implementation uses `canvas.WorldToScreen()` (2D). Refactor to use `EditorProjection::WorldToScreen()`:

```cpp
uint32_t HitTestNode(const EditorProjection& proj, const WorldGraphData& graph,
                     uint32_t mapId, float sx, float sy, float radius = 10.0f) {
    uint32_t bestId = 0;
    float bestDist = radius;

    for (const auto& node : graph.GetNodes()) {
        if (node.mapId != mapId) continue;
        float nsx, nsy;
        if (!proj.WorldToScreen(node.x, node.y, node.z + 0.5f, nsx, nsy))
            continue;  // behind camera (3D only)

        float dx = nsx - sx, dy = nsy - sy;
        float dist = sqrtf(dx * dx + dy * dy);
        if (dist < bestDist) {
            bestDist = dist;
            bestId = node.id;
        }
    }
    return bestId;
}
```

**Note on radius**: In 2D, nodes are rendered as fixed-size screen circles (5-8px). In 3D, nodes are world-space circles (radius 2.1 yards) whose screen size varies with camera distance. The hit-test radius of 10px is a screen-space constant and works in both modes — it's the "clickable area" regardless of visual size. No change needed.

### 1.3 Refactor HitTestEdge for Projection-Agnostic Use

Same approach — replace `canvas.WorldToScreen` with `proj.WorldToScreen`:

```cpp
int HitTestEdge(const EditorProjection& proj, const WorldGraphData& graph,
                uint32_t mapId, float sx, float sy, float threshold = 8.0f) {
    int bestIdx = -1;
    float bestDist = threshold;

    for (size_t i = 0; i < graph.GetEdges().size(); i++) {
        const auto& e = graph.GetEdges()[i];
        auto* from = graph.GetNode(e.fromNode);
        auto* to   = graph.GetNode(e.toNode);
        if (!from || !to) continue;
        if (from->mapId != mapId && to->mapId != mapId) continue;

        float sx1, sy1, sx2, sy2;
        if (!proj.WorldToScreen(from->x, from->y, from->z + 0.5f, sx1, sy1)) continue;
        if (!proj.WorldToScreen(to->x, to->y, to->z + 0.5f, sx2, sy2)) continue;

        // Point-to-segment distance in screen space
        float dist = PointToSegmentDist(sx, sy, sx1, sy1, sx2, sy2);
        if (dist < bestDist) {
            bestDist = dist;
            bestIdx = (int)i;
        }
    }
    return bestIdx;
}
```

### 1.4 Box Selection in 3D

Box selection already works in screen space — the user drags a rectangle on screen, and all nodes whose **screen projection** falls inside the rectangle are selected. The only change is replacing `canvas.WorldToScreen` with `proj.WorldToScreen` in the node containment test.

The visual overlay (semi-transparent blue rectangle via `ImGui::GetForegroundDrawList()`) works identically in both 2D and 3D since it's drawn in screen space.

```cpp
// On box select release (existing code, adapted):
for (const auto& node : graph.GetNodes()) {
    if (node.mapId != mapId) continue;
    float nsx, nsy;
    if (!proj.WorldToScreen(node.x, node.y, node.z + 0.5f, nsx, nsy))
        continue;

    if (nsx >= minSx && nsx <= maxSx && nsy >= minSy && nsy <= maxSy) {
        selection.SelectNode(node.id);
    }
}
```

### 1.5 Integration into App::RenderFrame3D

Add `GraphEditor::ProcessInput()` call to `RenderFrame3D()`, routed to the active graph:

```cpp
// In App::RenderFrame3D(), after camera input, before rendering:
if (m_activeGraph != ActiveGraph::ReadOnly) {
    EditorProjection3D proj;
    proj.camera = &m_camera3d;

    auto& graph = (m_activeGraph == ActiveGraph::Road)
                  ? m_roadGraphData : m_graphData;
    auto& sel   = m_selection;
    auto& undo  = (m_activeGraph == ActiveGraph::Road)
                  ? m_roadUndoCtx : m_undoCtx;

    m_graphEditor.ProcessInput(proj, graph, sel, m_currentMapId,
                               undo, &m_terrainSampler);
}
```

**Camera input conflict prevention**: The 3D camera processes input (orbit, pan) on middle-mouse / right-mouse. Graph editing uses left-mouse. These don't conflict. However, we must ensure that:
- When the user is box-selecting (left-drag), camera orbit is NOT triggered
- When graph context menu is open, camera input is suppressed
- `io.WantCaptureMouse` from ImGui panels prevents graph editing on panel areas

The existing `mouseInViewport && !io.WantCaptureMouse` guard pattern (already used in RenderFrame3D for Ctrl+Click) should be applied to `GraphEditor::ProcessInput`.

### 1.6 Files to Modify

| File | Changes |
|------|---------|
| `src/editor/graph_editor.h` | Add `EditorProjection` interface + 2D/3D structs, update `ProcessInput` signature |
| `src/editor/graph_editor.cpp` | Replace all `canvas.WorldToScreen`/`canvas.ScreenToWorld` with `proj.WorldToScreen`/`proj.ScreenToWorldXY` |
| `src/app.cpp` | Add `ProcessInput` call in `RenderFrame3D`, create `EditorProjection3D` |

### 1.7 Verification

- [ ] In 3D mode + Road Graph active: click on a node → selected (yellow highlight in 3D renderer)
- [ ] Click on empty space → selection cleared
- [ ] Shift+click → toggles node in/out of selection
- [ ] Click on edge → edge selected
- [ ] Box select (left-drag on empty space) → rectangle drawn, nodes inside selected on release
- [ ] Shift+box-select → adds to existing selection
- [ ] Ctrl+A → all nodes on map selected
- [ ] Escape → clears selection
- [ ] Property panel reflects 3D selections correctly
- [ ] Camera orbit (right-drag) NOT disrupted by graph editing
- [ ] Clicking on ImGui panels does NOT trigger graph editing

---

## Phase 2: 3D Node Dragging (XY-Plane Constrained) + Snap-to-Nearest

**Goal**: Drag selected nodes in 3D, constrained to the XY horizontal plane. Z remains unchanged.

### 2.1 XY-Plane Constrained Drag

When the user clicks on a selected node and drags in 3D mode, we need to compute the world-space XY delta. The approach:

1. On drag start: record the **reference Z** = `draggedNode.z` (the Z of the node being dragged)
2. Use `proj.ScreenToWorldXY(mouseX, mouseY, refZ, wx, wy)` to convert the current mouse position to world XY coordinates on the horizontal plane at `refZ`
3. Compute `deltaX = wx - anchorWx`, `deltaY = wy - anchorWy` (same as 2D)
4. Apply delta to all selected nodes' X and Y coordinates. **Do NOT modify Z.**

This is why `ScreenToWorldXY` takes a `refZ` parameter — it intersects the ray with the horizontal plane at the dragged node's height, giving accurate XY positioning regardless of camera angle.

```cpp
// In ProcessInput, during drag (adapted for 3D):
float wx, wy;
if (proj.ScreenToWorldXY(mx, my, m_drag.referenceZ, wx, wy)) {
    float deltaX = wx - m_drag.anchorWx;
    float deltaY = wy - m_drag.anchorWy;

    for (auto& [id, startPos] : m_drag.startPositions) {
        auto* node = graph.GetNode(id);
        if (node) {
            node->x = startPos.first + deltaX;
            node->y = startPos.second + deltaY;
            // node->z is NOT modified
        }
    }
    graph.MarkDirty();
}
```

**DragState extension**:
```cpp
struct DragState {
    // ... existing fields ...
    float referenceZ = 0.0f;  // Z of the dragged node for plane intersection
};
```

On drag start, set `m_drag.referenceZ = draggedNode->z`.

### 2.2 Snap-to-Nearest in 3D

Snap indicator works identically — find the nearest unselected node within 15px screen-space. The visual indicator (yellow line + circle) is drawn via `ImGui::GetForegroundDrawList()` using projected screen coordinates from `proj.WorldToScreen()`.

Ctrl+drag snap: snaps the dragged node's XY to the nearest unselected node's XY. Z is NOT snapped — only X and Y are affected.

### 2.3 Files to Modify

| File | Changes |
|------|---------|
| `src/editor/graph_editor.h` | Add `referenceZ` to `DragState` |
| `src/editor/graph_editor.cpp` | Drag logic uses `proj.ScreenToWorldXY(refZ)`, Z stays constant |

### 2.4 Verification

- [ ] Select nodes in 3D → drag → nodes move horizontally (XY only), Z unchanged
- [ ] Multi-node drag: all selected nodes maintain relative positions
- [ ] Ctrl+Z restores dragged nodes to original positions (including Z)
- [ ] Snap indicator appears when dragging near an unselected node
- [ ] Ctrl+drag snaps XY to nearest unselected node
- [ ] Dragging works at various camera angles (top-down, oblique, near-horizontal)
- [ ] Near-horizontal camera angles: drag still works correctly (plane intersection may be imprecise at very shallow angles — acceptable)

---

## Phase 3: 3D Context Menu + Core Operations

**Goal**: Right-click context menu in 3D with all operations from the 2D editor.

### 3.1 Context Menu in 3D

The context menu popup is rendered via ImGui (`ImGui::BeginPopup("GraphContextMenu")`), which is screen-space. It works identically in 2D and 3D. The trigger (right-click) and hit testing use the same `proj.WorldToScreen`-based methods.

All context menu items from 2D are available in 3D:

```
Context menu structure (identical to 2D):
┌──────────────────────────────┐
│ Delete (N nodes, M edges)    │  ← if selection non-empty
│ ──────────────────────────── │
│ Connect Nodes                │  ← if exactly 2 nodes selected
│ Split Edge                   │  ← if edge under cursor or single edge selected
│ Merge Nodes                  │  ← if 2+ nodes selected
│ Straighten Path              │  ← if 3+ connected chain nodes selected
│ ──────────────────────────── │
│ Add Node Here                │  ← always available
│ ──────────────────────────── │
│ Auto-Connect Endpoints...    │
│ Validate Graph...            │
│ ──────────────────────────── │
│ Select All                   │
│ Deselect All                 │
└──────────────────────────────┘
```

### 3.2 "Add Node Here" in 3D

When the user right-clicks on empty space and selects "Add Node Here", we need to determine the world position. Use `proj.ScreenToWorldXY(clickX, clickY, refZ, wx, wy)` where `refZ` is:

- If terrain height sampler is available: compute XY first with `refZ = camera.targetZ`, then sample `z = sampler->SampleHeight(mapId, wx, wy)` for the actual terrain Z
- If no sampler: use `camera.targetZ` (same as current Ctrl+Click teleport behavior)

This gives a reasonable world position for the new node.

### 3.3 Split Edge in 3D

Split edge inserts a node at the cursor's projection onto the edge. In 3D, the cursor world position comes from `proj.ScreenToWorldXY()` with `refZ` taken from the edge midpoint Z. The new node's Z is interpolated from the edge endpoints.

Alternatively, use the simpler approach: split at the **parametric position** along the edge (the `t` value from the screen-space point-to-segment projection). This gives the exact point on the edge closest to the cursor in screen space, and the Z is linearly interpolated:

```cpp
// t = parametric position along edge (0 = fromNode, 1 = toNode)
float t = PointToSegmentT(cursorSx, cursorSy, fromSx, fromSy, toSx, toSy);
t = std::clamp(t, 0.05f, 0.95f);  // avoid creating node at exact endpoint

mid.x = from->x + t * (to->x - from->x);
mid.y = from->y + t * (to->y - from->y);
mid.z = from->z + t * (to->z - from->z);
```

### 3.4 Operations That Need No Changes

The following operations are purely data-level — they modify `WorldGraphData` and `MultiSelection` without any coordinate conversion. They work identically in 2D and 3D with zero code changes:

- **Delete selected** — removes nodes/edges by ID
- **Connect nodes** — creates edge between 2 selected nodes
- **Merge nodes** — centroid computation + edge remapping
- **Straighten path** — BFS chain + linear interpolation of XY (and Z)
- **Auto-connect endpoints** — distance-based edge creation
- **Validate graph** — BFS components, degree analysis
- **Bulk property edit** — property panel, already works in 3D
- **Undo/redo** — snapshot-based, data-level only

### 3.5 Files to Modify

| File | Changes |
|------|---------|
| `src/editor/graph_editor.cpp` | "Add Node Here" uses `proj.ScreenToWorldXY`; split edge uses parametric position |

### 3.6 Verification

- [ ] Right-click in 3D viewport → context menu appears
- [ ] "Add Node Here" creates node at correct world XY position with terrain Z
- [ ] "Delete" removes selected nodes/edges, Ctrl+Z restores
- [ ] "Connect Nodes" creates edge between 2 selected nodes
- [ ] "Split Edge" inserts node at cursor position along edge
- [ ] "Merge Nodes" merges 2+ nodes at centroid
- [ ] "Straighten Path" aligns intermediate nodes along line
- [ ] "Auto-Connect Endpoints" finds and connects nearby endpoints
- [ ] "Validate Graph" reports disconnected components, dead-ends, orphans
- [ ] All context menu operations support Ctrl+Z undo

---

## Phase 4: 3D Edge Mode + Draw Mode

**Goal**: Enable Edge Mode (E key) and Draw Mode (D key) in 3D.

### 4.1 Edge Mode in 3D

Edge Mode allows clicking two nodes sequentially to create an edge between them. The logic is:

1. Press `E` to toggle edge mode
2. Click on first node → select it, store as `m_edgeStartNode`
3. Click on second node → create edge between `m_edgeStartNode` and clicked node

The ONLY coordinate-dependent operation is the **click-on-node** hit test, which already uses `proj.WorldToScreen()` after Phase 1. All other logic (edge creation, cost calculation, duplicate check) is data-level. Edge mode should work in 3D with no additional changes beyond Phase 1.

### 4.2 Draw Mode in 3D

Draw Mode (D key) allows rapid node placement by clicking sequentially. Each click:
1. Places a new node at the cursor position (or selects an existing node if clicked on one)
2. Connects the new node to the previous node with a Walk edge
3. Clicking on an existing edge splits it and continues the chain from the split point

**Coordinate dependencies**:
- **New node position**: use `proj.ScreenToWorldXY(mouseX, mouseY, refZ, wx, wy)` where `refZ` = terrain height or camera target Z. Then sample terrain height for Z via `TerrainHeightSampler::SampleHeight(mapId, wx, wy)`.
- **Click-on-existing-node**: uses `HitTestNode` (already adapted in Phase 1)
- **Click-on-existing-edge**: uses `HitTestEdge` (already adapted in Phase 1), then splits at parametric position (Phase 3)
- **Edge creation**: data-level, no changes needed

### 4.3 Draw Mode Visual Feedback in 3D

In 2D, draw mode shows a rubber-band line from the last placed node to the cursor. In 3D, this can be drawn via `Primitives3D::AddLine()` from the last node's 3D position to the cursor's projected world position:

```cpp
if (m_drawMode && m_drawLastNodeId != 0) {
    auto* lastNode = graph.GetNode(m_drawLastNodeId);
    if (lastNode) {
        float cursorWx, cursorWy;
        if (proj.ScreenToWorldXY(mx, my, lastNode->z, cursorWx, cursorWy)) {
            // Draw rubber-band line in 3D
            prims.AddLine(lastNode->x, lastNode->y, lastNode->z + 0.5f,
                          cursorWx, cursorWy, lastNode->z + 0.5f,
                          IM_COL32(0, 255, 0, 150));
        }
    }
}
```

Alternatively, draw the rubber-band line in screen space via `ImGui::GetForegroundDrawList()` using projected endpoints (simpler, no need to pass `Primitives3D` to `GraphEditor`).

**Decision**: Use screen-space drawing via `ImGui::GetForegroundDrawList()` for rubber-band lines. This keeps `GraphEditor` independent of `Primitives3D` and works in both 2D and 3D modes. The line is drawn from `proj.WorldToScreen(lastNode)` to the current mouse position.

### 4.4 Delete Node in Draw Mode (Right-Click)

In 2D Draw Mode, right-clicking a node deletes it immediately. This uses `HitTestNode` (already adapted) and `graph.RemoveNode()` (data-level). Works in 3D with no changes.

### 4.5 S Key to Split Edge at Cursor

In 2D, pressing `S` near an edge splits it at the cursor position. This uses `HitTestEdge` (already adapted) and the split logic from Phase 3. Works in 3D with no changes.

### 4.6 Files to Modify

| File | Changes |
|------|---------|
| `src/editor/graph_editor.cpp` | Draw mode rubber-band: use `proj.WorldToScreen` for line endpoints; new node creation uses `proj.ScreenToWorldXY` |

### 4.7 Verification

- [ ] Press E in 3D → enter edge mode, indicator shown
- [ ] Click node A, click node B → edge created between A and B
- [ ] Press E again → exit edge mode
- [ ] Press D in 3D → enter draw mode, indicator shown
- [ ] Click empty space → new node placed at correct world position, connected to previous
- [ ] Click existing node → chain continues from that node
- [ ] Click existing edge → edge split, chain continues from split point
- [ ] Rubber-band line visible from last node to cursor
- [ ] Right-click node in draw mode → node deleted immediately
- [ ] Press S near edge → edge split at cursor position
- [ ] Press D again → exit draw mode

---

## Phase 5: 3D Lasso Selection + Keyboard Shortcuts + Full Parity Verification

**Goal**: Complete feature parity between 2D and 3D graph editing.

### 5.1 Lasso Selection in 3D

Lasso selection (Alt+left-drag) draws a freeform polygon on screen and selects all nodes whose screen projections fall inside. This is inherently a screen-space operation:

1. Capture lasso polygon points in screen coordinates (identical to 2D)
2. On release, project each node to screen via `proj.WorldToScreen(node.x, node.y, node.z + 0.5f, sx, sy)`
3. Test if `(sx, sy)` is inside the lasso polygon using the existing ray-casting point-in-polygon test
4. Select nodes that pass the test

The visual overlay (polyline on `GetForegroundDrawList()`) is already screen-space and works in both modes.

**Shift+lasso**: adds to existing selection (same as 2D).

### 5.2 Keyboard Shortcuts in 3D

All keyboard shortcuts must work in 3D mode when `m_activeGraph != ReadOnly`:

| Key | Action | 3D Notes |
|-----|--------|----------|
| `E` | Toggle edge mode | Works — no coordinate dependency |
| `D` | Toggle draw mode | Works — node creation uses `proj.ScreenToWorldXY` |
| `S` | Split edge at cursor | Works — uses adapted `HitTestEdge` |
| `Delete` | Delete selected | Works — data-level operation |
| `Ctrl+A` | Select all nodes on map | Works — data-level operation |
| `Escape` | Clear selection / exit modes | Works — data-level operation |
| `Ctrl+Z` | Undo | Works — routed to active graph's undo stack |
| `Ctrl+Shift+Z` | Redo | Works — routed to active graph's undo stack |
| `Alt+Drag` | Lasso selection | Phase 5.1 |

**Conflict check**: In 3D mode, some keys may be bound to camera controls. Verify:
- `E/D/S` are NOT used by Camera3D (camera uses mouse buttons + WASD in free-fly, but free-fly mode is a separate toggle)
- If Camera3D free-fly uses WASD, then `D` and `S` conflict → resolve by disabling camera free-fly keys when `m_activeGraph != ReadOnly`, or require a modifier key for camera movement

### 5.3 Double-Click to Create Node

In 2D, double-clicking empty space creates a node. This should work in 3D:
- Detect `ImGui::IsMouseDoubleClicked(Left)` on empty space (no hit test match)
- Create node at `proj.ScreenToWorldXY(mouseX, mouseY, refZ, wx, wy)` with terrain-sampled Z
- Same as "Add Node Here" from context menu (Phase 3.2)

### 5.4 Camera Input Conflict Resolution

When graph editing is active in 3D, prevent mouse interactions from simultaneously controlling the camera and the graph:

```cpp
// In RenderFrame3D():
bool graphEditorWantsInput = (m_activeGraph != ActiveGraph::ReadOnly) &&
                             mouseInViewport && !io.WantCaptureMouse;

// Process camera input: suppress left-button orbit if graph editor is active
m_camera3d.ProcessInput(/* suppressLeftButton = graphEditorWantsInput */);

// Then process graph editor input
if (graphEditorWantsInput) {
    EditorProjection3D proj{ &m_camera3d };
    m_graphEditor.ProcessInput(proj, ...);
}
```

**Camera control in editing mode**:
- **Right-click**: Camera orbit (always available; graph editor uses left-click for selection, right-click opens context menu only, which is intercepted by ImGui popup)
- **Middle-click**: Camera pan (always available)
- **Scroll**: Camera zoom (always available)
- **Left-click**: Graph editing (selection, drag, edge mode, draw mode)

If Camera3D currently uses left-click for anything (e.g., orbit), it must yield to the graph editor when `m_activeGraph != ReadOnly`. Verify Camera3D controls and adjust if needed.

### 5.5 Inactive Graph Rendering in 3D

Currently in 3D mode, the inactive graph is NOT rendered (unlike 2D which shows it as a dim overlay). For consistency with 2D, render the inactive graph in 3D as a dim overlay:

```cpp
// In RenderFrame3D() when ActiveGraph::Road:
// Render world graph with reduced alpha (dim overlay)
m_graph3DRenderer.Render(prims, camera, m_graphData, m_emptySelection,
                         m_currentMapId, 0.3f /* dimAlpha */);
// Render road graph with full selection
m_graph3DRenderer.Render(prims, camera, m_roadGraphData, m_selection,
                         m_currentMapId, 1.0f);
```

Requires adding an alpha/dim parameter to `Graph3DRenderer::Render()`.

### 5.6 Full Feature Parity Checklist

Every 2D road graph editing feature and its 3D status:

| Feature | 2D | 3D Status |
|---------|----|-----------|
| Single-click node select | Yes | Phase 1 |
| Shift+click toggle select | Yes | Phase 1 |
| Single-click edge select | Yes | Phase 1 |
| Box selection (drag) | Yes | Phase 1 |
| Shift+box select (add) | Yes | Phase 1 |
| Lasso selection (Alt+drag) | Yes | Phase 5 |
| Shift+lasso (add) | Yes | Phase 5 |
| Ctrl+A select all | Yes | Phase 1 |
| Escape clear selection | Yes | Phase 1 |
| Multi-node drag | Yes | Phase 2 |
| XY-only drag (Z unchanged) | N/A (2D) | Phase 2 |
| Ctrl+drag snap to nearest | Yes | Phase 2 |
| Right-click context menu | Yes | Phase 3 |
| Delete selected (Delete key) | Yes | Phase 3 |
| Connect 2 nodes (context menu) | Yes | Phase 3 |
| Split edge (context menu + S key) | Yes | Phase 3 |
| Add node (double-click / context) | Yes | Phase 3 |
| Merge nodes (context menu) | Yes | Phase 3 |
| Straighten path (context menu) | Yes | Phase 3 |
| Auto-connect endpoints | Yes | Phase 3 |
| Validate graph | Yes | Phase 3 |
| Edge mode (E key) | Yes | Phase 4 |
| Draw mode (D key) | Yes | Phase 4 |
| Draw mode rubber-band line | Yes | Phase 4 |
| Draw mode right-click delete | Yes | Phase 4 |
| Draw mode split-on-edge-click | Yes | Phase 4 |
| Property panel editing | Yes | Already works |
| Undo/redo (Ctrl+Z/Ctrl+Shift+Z) | Yes | Already works |
| Bulk property edit | Yes | Already works |
| Road graph save | Yes | Already works |

### 5.7 Files to Modify

| File | Changes |
|------|---------|
| `src/editor/graph_editor.cpp` | Lasso uses `proj.WorldToScreen` for containment test |
| `src/app.cpp` | Camera/editor input conflict resolution, inactive graph dim rendering |
| `src/render/graph_renderer_3d.h` | Add alpha/dim parameter to `Render()` |
| `src/render/graph_renderer_3d.cpp` | Apply alpha/dim to colors |
| `src/camera/camera3d.h` | Optional: add `suppressLeftButton` parameter to `ProcessInput` |
| `src/camera/camera3d.cpp` | Optional: implement left-button suppression |

### 5.8 Verification

- [ ] Alt+drag in 3D → lasso selection works, freeform polygon drawn on screen
- [ ] Shift+lasso → adds to existing selection
- [ ] Double-click empty space in 3D → new node created at correct position
- [ ] All keyboard shortcuts work in 3D mode (E, D, S, Delete, Ctrl+A, Escape, Ctrl+Z)
- [ ] Camera orbit/pan/zoom still works while graph editing is active (using right/middle mouse)
- [ ] Left-click is exclusively used for graph editing when `ActiveGraph != ReadOnly`
- [ ] Inactive graph rendered as dim overlay in 3D
- [ ] **Full parity test**: perform every 2D editing operation in 3D and verify identical behavior

---

## Implementation Order

```
Phase 1: 3D Hit Testing + Selection
    ↓    (EditorProjection abstraction — foundational refactor)
Phase 2: 3D Node Dragging + Snap
    ↓    (requires Phase 1 for hit testing on drag start)
Phase 3: 3D Context Menu + Operations
    ↓    (requires Phase 1 for hit testing, Phase 2 for ScreenToWorldXY)
Phase 4: 3D Edge Mode + Draw Mode
    ↓    (requires Phase 1-3 for hit testing, node creation, edge split)
Phase 5: 3D Lasso + Polish + Parity
         (requires all above for final verification)
```

Each phase builds on the previous. Phase 1 is the critical foundation — the `EditorProjection` abstraction enables all subsequent phases. Phases 2-4 are relatively independent of each other but all depend on Phase 1. Phase 5 is polish and verification.

---

## Key Design Decisions

### Why EditorProjection abstraction instead of duplicating GraphEditor

Duplicating `GraphEditor` for 3D would mean ~1100 lines of near-identical code with only projection-related differences. The `EditorProjection` interface isolates the 5-6 methods that differ between 2D and 3D into a clean abstraction. This avoids code duplication, ensures bug fixes apply to both modes, and makes adding future features (e.g., orthographic 3D) trivial.

### Why XY-plane drag instead of screen-plane drag

Screen-plane drag (moving nodes parallel to the screen) would cause unexpected Z changes when the camera is tilted. XY-plane drag is predictable: nodes always move horizontally, matching the user's mental model of "sliding nodes across the map." The user explicitly stated Z editing should be done through the Property Panel only.

### Why screen-space hit testing instead of ray-object intersection

True ray-circle/ray-line intersection in 3D would be mathematically precise but unnecessarily complex. Since graph nodes are rendered as flat circles facing up (XY plane), and the user perceives them in screen space, projecting to screen and doing 2D distance checks is simpler, faster, and gives the expected UX. This is the same approach used by professional 3D editors (e.g., Blender's gizmo hit testing).

### Why ImGui draw list for visual feedback instead of Primitives3D

Box selection rectangles, lasso outlines, snap indicator lines, and rubber-band lines are all screen-space overlays. Using `ImGui::GetForegroundDrawList()` is the natural choice — it's already used for these purposes in 2D mode, it doesn't require passing `Primitives3D` to `GraphEditor`, and it keeps the editor decoupled from the rendering backend.

---

## Out of Scope

- Z-axis editing via node dragging (by design — use Property Panel)
- Ray-vs-terrain intersection for precise node placement (ground-plane intersection + terrain height sampling is sufficient)
- 3D gizmos (translate/rotate/scale handles) — not needed for graph editing
- 3D edge mode visual preview (showing a preview edge from first node to cursor in world space) — screen-space rubber-band is sufficient
- Free-fly camera WASD controls during editing (disabled to avoid key conflicts)

---

## Risks and Mitigations

| Risk | Impact | Mitigation |
|------|--------|------------|
| `ProcessInput` refactor breaks 2D editing | High | `EditorProjection2D` wraps Canvas identically; run all 2D tests after refactor |
| Near-horizontal camera angle causes imprecise drag | Low | `ScreenToWorldXY` returns false when ray is near-parallel to plane; drag stops until angle improves |
| Camera/editor input conflicts | Medium | Clear separation: left-click = editor, right/middle = camera; ImGui popup blocks both |
| Performance of screen-space hit testing with many nodes | Low | Same O(N) scan as 2D; road graphs have ~2000 nodes, negligible cost |
