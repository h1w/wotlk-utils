# Center-based radial navmesh tile loading

**Date:** 2026-02-22

## Summary

Replaced viewport-rectangle tile loading with a center-based radial system. Tiles now load from the screen center outward, up to 200 tiles max. Removed LRU eviction machinery in favor of explicit per-frame wanted-set diffing.

## Problem

The previous system tried to load all tiles within the viewport rectangle. This was fragile — tiles appeared only in parts of the screen or outside the viewport entirely. When zoomed out, the tile count could exceed the cache limit, causing the entire update to bail out (`if (tileCount > kMaxCachedTiles) return`).

## Solution

### Radial loading (`TileCache::UpdateViewport`)

1. Compute the **center tile** from `canvas.centerX` / `canvas.centerY`
2. Compute the viewport tile range (same bounding formula as before)
3. Build a list of all tiles in the viewport range, sorted by **squared distance from center** (nearest first)
4. Take the first **200** tiles — this is the "wanted set"
5. **Evict** any cached tile not in the wanted set (remove from `dtNavMesh` via stored `tileRef`)
6. **Load** any wanted tile not yet cached, in radial order

### Removed LRU machinery

The LRU list is no longer needed — the radial system explicitly decides which tiles to keep each frame. Removed `m_lruList`, `TouchTile()`, `EvictLRU()`. Simplified `m_cache` from `std::map<Key, pair<Entry, list::iterator>>` to `std::map<Key, Entry>`.

### Simplified renderer

`NavmeshRenderer::Render` no longer computes its own viewport tile range. It uses the new `TileCache::ForEachTile(callback)` method to iterate all cached tiles directly, since `UpdateViewport` already guarantees only valid near-center tiles are in the cache.

## Files changed

| File | Change |
|------|--------|
| `src/navmesh/tile_cache.h` | `kMaxCachedTiles` 600→200, removed `m_lruList`/`EvictLRU`/`TouchTile`, simplified `m_cache` type, added `ForEachTile()` |
| `src/navmesh/tile_cache.cpp` | Rewrote `UpdateViewport` with radial sort + wanted-set diffing, removed `EvictLRU`/`TouchTile`, simplified `LoadTile`/`Clear` |
| `src/render/navmesh_renderer.cpp` | Removed viewport-to-tile-range calculation, uses `ForEachTile` instead |
