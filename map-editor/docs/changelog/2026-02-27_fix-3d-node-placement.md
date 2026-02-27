# Fix 3D Node Placement (Ray-Terrain Intersection)

**Date:** 2026-02-27

## Summary

Fixed a bug where creating nodes in 3D mode (draw mode, context menu, double-click) placed them at the wrong position — shifted "higher and further" from the actual click point. The root cause was twofold: (1) ray-plane intersection used Z=0 instead of the actual terrain height, and (2) flat-plane intersection cannot handle sloped terrain. Replaced with a proper ray-terrain intersection via ray marching + binary search.

## Problem

`EditorProjection3D::ScreenToWorldXY` converts screen clicks to world coordinates by casting a ray from the camera and intersecting it with a horizontal plane at `refZ`. Three issues:

1. **refZ=0 for all node creation paths** — draw mode (first node), context menu (no node under cursor), double-click. WoW terrain is typically at Z=100-200+, so intersecting at Z=0 caused the ray to overshoot far past the visible terrain, giving displaced XY coordinates.

2. **Flat-plane intersection fails on slopes** — even with a correct `refZ` (e.g., `camera->targetZ`), a horizontal plane cannot model sloped terrain. On a mountainside, the intersection point slides uphill/downhill from the actual click position.

3. **Status bar already had the correct approach** — `status_bar.cpp` used `camera.targetZ` as the intersection plane, but `graph_editor.cpp` used hardcoded `0.0f`.

## Solution

### Phase 1: GetDefaultRefZ (partial fix)

Added `EditorProjection::GetDefaultRefZ()` virtual method:
- 2D: returns `0.0f` (unchanged)
- 3D: returns `camera->targetZ`

Replaced all `refZ = 0.0f` in node creation paths with `proj.GetDefaultRefZ()`. This fixed placement on flat terrain but not on slopes.

### Phase 2: Ray-Terrain Intersection (full fix)

Added `EditorProjection::ScreenToWorldOnTerrain()` virtual method with ray marching:

1. Cast ray from camera through clicked pixel via `ScreenToRay`
2. March along the ray in 256 steps from near plane to far plane
3. At each step, compare ray Z with actual terrain Z from `TerrainHeightSampler::SampleHeight`
4. When the ray crosses from above to below terrain surface — binary search (16 iterations) for the exact intersection point
5. Fallback to flat-plane at `targetZ` if no terrain data is loaded

`EditorProjection3D` now carries `TerrainHeightSampler*` and `mapId` to enable terrain queries. These are set when creating the projection in `app.cpp`.

Node creation paths (draw mode, context menu, double-click) call `ScreenToWorldOnTerrain`. Node dragging continues to use `ScreenToWorldXY` with a fixed reference Z (correct behavior — drag stays on a horizontal plane at the node's height).

## Changes

### `graph_editor.h`

- Added `GetDefaultRefZ()` virtual method to `EditorProjection` (returns 0 for 2D, `camera->targetZ` for 3D)
- Added `ScreenToWorldOnTerrain()` virtual method with default implementation delegating to `ScreenToWorldXY`
- Added `heightSampler` and `mapId` fields to `EditorProjection3D`
- Added override declarations for both new methods in `EditorProjection3D`

### `graph_editor.cpp`

- Implemented `EditorProjection3D::GetDefaultRefZ()` — returns `camera->targetZ`
- Implemented `EditorProjection3D::ScreenToWorldOnTerrain()` — ray marching (256 steps) + binary search (16 iterations) against terrain height data
- **Draw mode** (line ~1000): replaced `ScreenToWorldXY(mx, my, drawRefZ, ...)` with `ScreenToWorldOnTerrain(mx, my, ...)`
- **Context menu** (line ~1144): replaced `ScreenToWorldXY(mx, my, refZ, ...)` with `ScreenToWorldOnTerrain(mx, my, ...)`
- **Double-click** (line ~1230): replaced `ScreenToWorldXY(mx, my, 0.0f, ...)` with `ScreenToWorldOnTerrain(mx, my, ...)`
- Node dragging unchanged — still uses `ScreenToWorldXY` with `m_drag.referenceZ`

### `app.cpp`

- Set `proj3d.heightSampler = &m_heightSampler` and `proj3d.mapId = m_currentMapId` when creating `EditorProjection3D`

## Modified Files

| File | Changes |
|------|---------|
| `src/editor/graph_editor.h` | `GetDefaultRefZ()`, `ScreenToWorldOnTerrain()` virtuals; `heightSampler`/`mapId` on 3D struct |
| `src/editor/graph_editor.cpp` | Ray-terrain intersection implementation; 3 call sites updated to use terrain picking |
| `src/app.cpp` | Pass `heightSampler` and `mapId` to `EditorProjection3D` |
