# Fix navmesh disappearing at high zoom — tile coordinate off-by-one

**Date:** 2026-02-22

## Summary

Fixed a critical one-tile offset in the navmesh tile coordinate mapping that caused navmesh polygons (and all tile-based rendering) to disappear when zooming in past ~2x. The actual navmesh vertex data for each tile was shifted by exactly 533 yards (one tile) from where the TileCache assumed it was. At high zoom, the loaded tiles' data fell entirely outside the viewport.

## Problem

When zooming in beyond ~2x, navmesh polygons disappeared completely. At low zoom (0.1-0.5x), everything looked correct because many tiles were loaded simultaneously, masking the positional error.

**Root cause:** The tile coordinate formula `tileX = 32 - floor(wowX / 533.33)` assumed each tile covered `wowX in [(32-tileX)*533, (33-tileX)*533)`. But the actual navmesh vertex data (from TC-generated `.mmtile` files) covered `wowX in [(31-tileX)*533, (32-tileX)*533)` — shifted by exactly one tile in both X and Y axes.

At zoom 8.76x, for example:
- Viewport covered wowX = [1635, 1782]
- Loaded tile (29, 34) had vertex data at wowX = [1067, 1600]
- **Zero overlap** — the navmesh data was entirely outside the viewport

## Diagnosis

Added AABB diagnostic logging to `NavmeshRenderer` that recorded the actual vertex coordinate ranges for each GPU-cached tile. Cross-referencing these with the viewport bounds and tile coordinate formula revealed the systematic offset:

```
Tile (31,32): AABB wowX=[0, 533]       expected=[ 533, 1067]  shift=-533
Tile (29,34): AABB wowX=[1067, 1600]   expected=[1600, 2133]  shift=-533
Tile (28,33): AABB wowX=[1600, 2133]   expected=[2133, 2667]  shift=-533
Tile (23,37): AABB wowX=[4267, 4400]   expected=[4800, 5333]  shift=-533
```

Same -533 shift in both wowX and wowY for every tile examined.

Also added bright magenta ImGui debug rectangles at each tile's AABB position. Both the rectangles AND the navmesh disappeared when zooming in, confirming the issue was in coordinate mapping (not the DX11 rendering pipeline).

## Fix

Changed the tile coordinate formula from `32 - floor(...)` to `31 - floor(...)` across all tile-related code in the map editor. Updated tile world bounds from `(32-tx)*TS..(33-tx)*TS` to `(31-tx)*TS..(32-tx)*TS`.

```cpp
// Before (incorrect — tile data was one tile away from viewport):
int centerTX = 32 - static_cast<int>(std::floor(canvas.centerX / kTileSize));

// After (correct — tiles now loaded for the area they actually cover):
int centerTX = 31 - static_cast<int>(std::floor(canvas.centerX / kTileSize));
```

Note: `minimap_cache.cpp` was already using the correct `(31-tx)` formula — it had been independently fixed in the 2026-02-21 map orientation patch. The navmesh, grid, and UI code were not updated at that time.

## Additional cleanup

- Removed CPU-side triangle culling path (`needCull` branch, `UpdateDynamicVB`, `m_dynamicVB`) that was added as an earlier attempted fix. It rejected ALL triangles at zoom >= 15 due to the same coordinate mismatch
- Removed magenta debug AABB rectangles from `NavmeshRenderer::Render`
- Restored navmesh fill alpha from 180/255 (debug visibility) back to 40/255

## Why it was invisible at low zoom

At zoom 0.1x, the viewport covers ~12000 yards. The TileCache loads 200+ tiles spanning this area. Even though each tile's data was shifted by 533 yards, the aggregate coverage still filled the viewport — the "wrong" tiles for a given viewport position were compensated by adjacent "wrong" tiles whose shifted data happened to land in the right place. Only at high zoom (small viewport, 1-4 tiles loaded) did the shift cause tiles to fall completely outside the visible area.

## Files changed

| File | Change |
|------|--------|
| `src/navmesh/tile_cache.cpp` | `32 - floor(...)` -> `31 - floor(...)` for center tile and viewport tile range |
| `src/navmesh/tile_loader.cpp` | `WowTileToDetourTile` center: `(32 - t + 0.5)` -> `(31 - t + 0.5)` |
| `src/render/navmesh_renderer.cpp` | Removed debug AABB rects and CPU-cull path, restored fill alpha |
| `src/render/navmesh_renderer.h` | Removed `m_dynamicVB`, `m_dynamicVBSize`, `m_culledVerts`, `UpdateDynamicVB` |
| `src/render/grid_renderer.cpp` | Tile range `31 - floor(...)`, bounds `(31-tx)*TS..(32-tx)*TS` |
| `src/ui/status_bar.cpp` | Cursor tile coordinate: `31 - floor(...)` |
| `src/ui/property_panel.cpp` | Tile info lookup: `31 - floor(...)` |
| `src/app.cpp` | Background bounds: `(31 - tMaxX)*TS..(32 - tMinX)*TS` |

## Tile coordinate convention (corrected)

```
Tile (tileX, tileY) covers:
  wowX in [(31 - tileX) * 533.33, (32 - tileX) * 533.33)
  wowY in [(31 - tileY) * 533.33, (32 - tileY) * 533.33)

Reverse:
  tileX = 31 - floor(wowX / 533.33)
  tileY = 31 - floor(wowY / 533.33)
```
