# Fix pathfinder broken + tile loading regression after TASK_003

**Date:** 2026-02-23

## Summary

Fixed two regressions introduced after TASK_003 (terrain/building rendering): pathfinding completely broken (`findNearestPoly` returns 0 for every click) and navmesh tile loading ~10x slower than before.

## Bug Fixes

### 1. Pathfinder fails — stale accessOrder causes premature navmesh eviction

**Root cause:** `UpdateViewport` and `UpdateViewport3D` skip `EnsureTileInNavmesh` for tiles already in the rendering cache (`m_cache`). This means their `accessOrder` in `m_meshTiles` is never refreshed. When many tiles are loaded for rendering (200+ candidates after zooming out with terrain/buildings visible), the LRU eviction aggressively removes stale-accessOrder tiles from `dtNavMesh` — including the player's tile. Rendering continues fine (cached triangles are self-contained), but pathfinding fails (tile gone from dtNavMesh).

**Fix:** Added an accessOrder refresh loop before the rendering cache populate loop in both `UpdateViewport` and `UpdateViewport3D`. All wanted tiles already present in `m_meshTiles` get their `accessOrder` bumped, preventing premature eviction.

**Files:** `src/navmesh/tile_cache.cpp` — both `UpdateViewport` and `UpdateViewport3D`

### 2. Tile loading 10x slower — missing showNavmesh guard

**Root cause:** A previous fix removed the `m_layers.showNavmesh` guard from tile cache updates, causing navmesh tiles to load unconditionally even when the layer is hidden. This tripled disk I/O (navmesh + terrain + buildings all loading simultaneously).

**Fix:** Restored the `m_layers.showNavmesh &&` condition in both 2D and 3D tile cache update calls in `app.cpp`.

**Files:** `src/app.cpp` — 2D mode (line ~692) and 3D mode (line ~845)

### 3. Pathfinding unavailable when navmesh layer hidden

**Root cause:** With the `showNavmesh` guard restored, disabling the navmesh layer means no tiles are loaded into `dtNavMesh` at all — pathfinding stops working.

**Fix:** Added `TileCache::EnsurePathfindingTiles(posX, posY)` which loads a 3x3 tile area around a given position into `dtNavMesh`. Called from `RenderFrame3D` whenever the player marker is placed, regardless of navmesh layer visibility. This keeps pathfinding functional with minimal disk I/O (9 tiles max).

**Files:** `src/navmesh/tile_cache.h`, `src/navmesh/tile_cache.cpp`, `src/app.cpp`

## Files Changed

| File | Change |
|------|--------|
| `src/app.cpp` | Restored `showNavmesh` guard (2D+3D); added `EnsurePathfindingTiles` call |
| `src/navmesh/tile_cache.h` | Added `EnsurePathfindingTiles` declaration |
| `src/navmesh/tile_cache.cpp` | Added accessOrder refresh in both `UpdateViewport` methods; implemented `EnsurePathfindingTiles` |
