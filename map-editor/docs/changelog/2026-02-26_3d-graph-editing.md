# 3D Graph Editing (TASK-009)

**Date:** 2026-02-26
**Task:** TASK-009 (all 5 phases)

## Summary

Ported the full 2D road graph editing experience into 3D mode. Previously, 3D mode only visualized the graph — all editing had to be done in 2D. Now every editing operation works in 3D: selection (click, shift-click, box, lasso, Ctrl+A), multi-node drag (XY-plane constrained, Z unchanged), context menu, draw mode, edge mode, split/merge/straighten, auto-connect, validate, undo/redo. The implementation uses a polymorphic `EditorProjection` abstraction that routes coordinate transforms through either the 2D Canvas or the 3D Camera, keeping a single `GraphEditor` codebase for both modes with zero code duplication.

## Architecture

### EditorProjection Abstraction

Instead of duplicating the ~1100-line `GraphEditor` for 3D, a lightweight interface abstracts the projection differences:

```cpp
struct EditorProjection {
    virtual bool WorldToScreen(float wx, float wy, float wz,
                               float& sx, float& sy) const = 0;
    virtual bool ScreenToWorldXY(float sx, float sy, float refZ,
                                 float& wx, float& wy) const = 0;
    virtual bool IsInViewport(float sx, float sy) const = 0;
    virtual bool Is3D() const { return false; }
    virtual ~EditorProjection() = default;
};
```

Two implementations:
- **`EditorProjection2D`** — wraps `Canvas`, ignores Z in projection, always returns `true` for visibility
- **`EditorProjection3D`** — wraps `Camera3D`, uses `Camera3D::WorldToScreen` for projection and `Camera3D::ScreenToRay` + horizontal plane intersection for `ScreenToWorldXY`

`GraphEditor::ProcessInput` takes `const EditorProjection&` instead of `const Canvas&`. All hit tests, drag computations, node placement, and coordinate conversions go through this interface.

### 3D-Specific Behaviors

- **XY-plane constrained drag**: When dragging nodes in 3D, the mouse ray is intersected with a horizontal plane at the dragged node's Z height (`referenceZ`). Only X and Y are modified; Z is never changed by dragging.
- **Split edge parametric position**: In 3D, splitting an edge computes the parametric `t` from screen-space projection of the edge endpoints, then linearly interpolates world X, Y, Z along the edge. This gives the correct split point regardless of camera angle.
- **Shift+RMB for context menu / draw-mode delete**: In 3D mode, plain right-click is reserved for camera orbit. Context menu and draw-mode node deletion require **Shift+Right-Click**. In 2D mode, plain right-click works as before.
- **Inactive graph dim overlay**: When editing one graph (Road or World), the other is rendered in 3D at `dimAlpha = 0.3f`, matching the 2D dim overlay behavior.

## Changes

### EditorProjection Interface (`graph_editor.h`)

- Added `EditorProjection` base struct with `WorldToScreen`, `ScreenToWorldXY`, `IsInViewport`, `Is3D` virtual methods
- Added `EditorProjection2D` (wraps Canvas) and `EditorProjection3D` (wraps Camera3D)
- Changed `ProcessInput` signature from `const Canvas&` to `const EditorProjection&`
- Added `referenceZ` field to `DragState` for 3D plane intersection
- Added `IsLassoSelecting()` public method

### GraphEditor Refactoring (`graph_editor.cpp`)

- **All coordinate conversions** replaced: `canvas.WorldToScreen(x, y, sx, sy)` → `proj.WorldToScreen(x, y, z + 0.5f, sx, sy)` throughout the entire file
- **All screen-to-world** replaced: `canvas.ScreenToWorld(sx, sy, wx, wy)` → `proj.ScreenToWorldXY(sx, sy, refZ, wx, wy)` with appropriate reference Z
- **HitTestNode / HitTestEdge** now take `const EditorProjection&` and project through it
- **Drag**: sets `m_drag.referenceZ = hitNode->z` on drag start; uses `proj.ScreenToWorldXY(mx, my, referenceZ, ...)` for delta computation
- **Split edge (S key)**: uses parametric `t` from screen-space point-to-segment projection for X, Y, Z interpolation
- **Draw mode**: new node placement uses `proj.ScreenToWorldXY` with `drawRefZ` from last chain node's Z
- **Context menu trigger**: guarded with `!proj.Is3D() || shiftDown` — requires Shift+RMB in 3D
- **Draw mode right-click delete**: same Shift+RMB guard in 3D

### 3D Inactive Graph Overlay (`graph_renderer_3d.h`, `graph_renderer_3d.cpp`)

- Added `dimAlpha` parameter to `Graph3DRenderer::Render()` (default `1.0f`)
- Added `ApplyDim()` helper that scales the alpha channel of ABGR colors
- Dim applied to node and edge colors; selection highlights (white ring, yellow edges) are NOT dimmed

### App Integration (`app.cpp`)

- **2D path**: creates `EditorProjection2D` with `canvas` pointer, passes to `ProcessInput`
- **3D path**: creates `EditorProjection3D` with `camera3d` pointer, calls `ProcessInput` in `RenderFrame3D` when `m_activeGraph != ReadOnly` and `m_layers.showNodes`
- **Inactive graph rendering in 3D**: renders the non-active graph at `dimAlpha = 0.3f` using an empty selection

## Input Model in 3D

| Input | Action |
|-------|--------|
| Left-click | Select node/edge |
| Left-drag on node | Move selected nodes (XY only) |
| Left-drag on empty | Box selection |
| Shift+Left-click | Toggle node/edge in selection |
| Alt+Left-drag | Lasso selection |
| Shift+Right-click | Context menu |
| Shift+Right-click (draw mode) | Delete node under cursor |
| Right-drag | Camera orbit (unaffected by editing) |
| Middle-drag | Camera pan (unaffected by editing) |
| Scroll | Camera zoom (unaffected by editing) |

## Modified Files

| File | Changes |
|------|---------|
| `src/editor/graph_editor.h` | `EditorProjection` interface + 2D/3D structs, `ProcessInput` signature, `DragState.referenceZ`, `Is3D()` |
| `src/editor/graph_editor.cpp` | Full refactor: all projections via `EditorProjection`, Shift+RMB guards for 3D |
| `src/render/graph_renderer_3d.h` | `dimAlpha` parameter on `Render()` |
| `src/render/graph_renderer_3d.cpp` | `ApplyDim()` helper, dim applied to node/edge colors |
| `src/app.cpp` | 3D `ProcessInput` call, `EditorProjection2D`/`3D` usage, inactive graph dim rendering |
