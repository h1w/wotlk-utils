# Interactive Graph Editor (TASK-008)

**Date:** 2026-02-25
**Task:** TASK-008 (all 5 phases)

## Summary

Transformed the graph editing system from a basic single-node editor into a full interactive graph editor with multi-selection, context menus, draw mode, split/merge/straighten operations, lasso selection, graph validation, and three editing modes (Read Only, World Graph, Road Graph). Both world graph and road graph are now fully editable with independent undo/redo stacks.

## Changes

### Multi-Selection (Phase 1)

- **Replaced `Selection` with `MultiSelection`** — supports arbitrary numbers of selected nodes and edges simultaneously via `std::unordered_set`
- **Box selection** — left-click-drag on empty space draws a selection rectangle; all nodes inside are selected on release
- **Shift+Click** — toggle individual nodes/edges in/out of selection without clearing existing selection
- **Ctrl+A** — select all nodes on the current map
- **Escape** — clear selection
- **Delete key** — delete all selected nodes and edges (edges sorted descending to avoid index invalidation)
- **Property panel** — shows "N nodes, M edges selected" summary for multi-selection; single-selection shows full editor as before

### Context Menu (Phase 2)

- **Right-click context menu** — appears on right-click anywhere on the canvas
- **Menu items**: Delete Selection, Connect Nodes (exactly 2 selected), Split Edge (edge under cursor), Add Node Here, Merge Nodes (2+ selected), Straighten Path (3+ chain), Auto-Connect Nearby, Validate Graph, Select All, Deselect All
- **ImGui popup host window** — invisible `##GraphPopupHost` window provides required parent scope for `OpenPopup`/`BeginPopup` (called before `WantCaptureMouse` check)
- **Z-order fix** — graph/route/path overlay renderers changed from `GetForegroundDrawList()` to transparent overlay windows (`GetWindowDrawList()`) so context menu popup renders on top

### Multi-Node Drag (Phase 3)

- **Drag multiple nodes** — click-drag a selected node moves all selected nodes while maintaining relative positions
- **Undo snapshot** — single "Move N Nodes" snapshot on drag start; Ctrl+Z restores all to original positions
- **DragState struct** — tracks anchor position and start positions for all selected nodes

### Graph Operations (Phase 4)

- **Merge Nodes** — 2+ nodes selected: merges into one at centroid position, remaps all edges, removes self-loops and duplicates
- **Straighten Path** — 3+ connected chain nodes: lerps intermediates along line between endpoints via BFS chain ordering
- **Bulk property edit** — multi-select property panel: Type combo, Faction input, Edge Type, Bidirectional toggle, Recalc Costs (all apply to entire selection)

### Road Graph Editing & Modes (Phase 5)

- **Three editing modes** — `ReadOnly`, `World`, `Road` (cycled via toolbar button)
  - **Read Only** (grey, default) — world graph renders with full colors/labels, road graph as orange overlay; no editing, no undo/redo
  - **World Graph** (blue) — world graph is editable, road graph shown as orange overlay
  - **Road Graph** (orange) — road graph is editable, world graph shown as overlay
- **Independent undo/redo** — separate `UndoRedo` stacks for world graph and road graph; active stack follows editing mode
- **Save Road Graph** — menu items for Save / Save As (road graph JSON)
- **Lasso selection** — Alt+drag draws freeform polygon; point-in-polygon test selects enclosed nodes
- **Auto-connect nearby** — connects endpoint nodes (degree <= 1) within configurable distance
- **Graph validation** — detects disconnected components, dead-ends, duplicate edges, zero-length edges, orphan nodes; results popup with "Select Dead-ends" / "Select Orphans" / "Remove Duplicates" buttons

### Draw Mode (D key)

- **Draw mode** — press D to toggle; click to place nodes with auto-connect to previous node in chain
- **Click existing node** — when chain is active, connects chain to the clicked node (does not just set as new anchor)
- **Click on edge** — when chain is active and click lands on an edge, auto-splits the edge at cursor position and connects chain to the new split node
- **Right-click on node** — deletes node
- **Right-click on empty** — breaks chain (resets anchor)
- **Visual feedback** — green line from last node to cursor, green crosshair circle at cursor

### Split Edge (S key)

- **S key** — splits the nearest edge at cursor position (not midpoint); inserts new node at exact click location

### Help Panel (F1)

- **Help / Shortcuts window** — F1 or View > Help / Shortcuts; comprehensive reference listing all keyboard shortcuts and editing features organized by category (Navigation, Selection, Node Editing, Draw Mode, Edge Mode, Context Menu, Undo/Save, Active Graph, Other)

### Minor Fixes

- **Empty node names** — new nodes from Add Node, Split Edge, and Draw Mode now have empty names (was "New Node" / "Split")
- **`showWorldGraph` persistence** — layer visibility toggle for world graph is now saved/loaded from `app_settings.json`

## Modified Files

| File | Changes |
|------|---------|
| `src/editor/selection.h` | Replaced `Selection` with `MultiSelection` (unordered_set-based) |
| `src/editor/graph_editor.h` | Added draw mode, lasso, box select, drag, context menu, validation state |
| `src/editor/graph_editor.cpp` | All editor logic: modes, operations, context menu, keyboard shortcuts |
| `src/render/graph_renderer.h` | `LayerVisibility` extended; `RenderRoadOverlay` declaration |
| `src/render/graph_renderer.cpp` | Overlay window rendering (not foreground draw list); road overlay |
| `src/render/route_renderer.cpp` | Changed to overlay window rendering |
| `src/render/path_renderer.cpp` | Changed to overlay window rendering |
| `src/render/graph_renderer_3d.h` | Forward decl for `MultiSelection` |
| `src/render/graph_renderer_3d.cpp` | Updated to use `MultiSelection` |
| `src/ui/property_panel.h` | `MultiSelection` parameter |
| `src/ui/property_panel.cpp` | Multi-selection summary, bulk property editing |
| `src/ui/main_menu.h` | Save Road Graph actions |
| `src/ui/main_menu.cpp` | Save Road Graph / Save Road Graph As menu items |
| `src/data/app_settings.h` | `showWorldGraph`, `roadGraphPath` |
| `src/data/app_settings.cpp` | Persist world graph visibility and road graph path |
| `src/app.h` | `ActiveGraph` enum (ReadOnly/World/Road), `m_roadUndoRedo`, `m_showHelp` |
| `src/app.cpp` | Mode switching, conditional rendering, input gating, help panel, toolbar |

## Keyboard Shortcuts Reference

| Key | Action |
|-----|--------|
| D | Toggle Draw Mode |
| E | Toggle Edge Mode |
| S | Split nearest edge at cursor |
| Delete | Delete selected nodes/edges |
| Ctrl+A | Select all nodes |
| Escape | Clear selection |
| Ctrl+Z | Undo |
| Ctrl+Shift+Z | Redo |
| Ctrl+S | Save |
| F1 | Help / Shortcuts |
| Alt+Drag | Lasso selection |
| Shift+Click | Add/remove from selection |
