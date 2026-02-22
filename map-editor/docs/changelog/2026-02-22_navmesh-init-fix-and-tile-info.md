# Fix navmesh init failure + tile info in Properties panel

**Date:** 2026-02-22

## Summary

Fixed a critical bug where `dtNavMesh::init` rejected all maps due to insufficient salt bits, and added a "Navmesh Tile" section to the Properties panel showing per-tile metadata on hover.

## Part 1: Navmesh init fix

### Problem

`CreateNavMesh` failed for every map with `Failed to create navmesh for map X`. Root cause: `kSaltBits=6` violated Detour 1.6.0's minimum of 10 salt bits (`DetourNavMesh.cpp:260`: `if (m_saltBits < 10) return DT_FAILURE | DT_INVALID_PARAM`).

With `kMaxTiles=512` (tileBits=9), raising saltBits to 10 would leave only `polyBits=13` (maxPolys=8192) — too few for dense city tiles (Stormwind, Orgrimmar have >8192 polys), causing `addTile` failures with status `0x80000008`.

### Solution

Reduced `kMaxTiles` from 512 to 64:

- `tileBits=6`, `saltBits=10`, `polyBits=16` → `maxPolys=65536` (covers all WotLK tiles)
- 64 navmesh slots is sufficient because the rendering cache (TileTriangles) is independent — once triangles are extracted, the navmesh tile can be evicted

Decoupled navmesh eviction from rendering cache: `EvictFarthestNavmeshTile` no longer calls `m_cache.erase()`. With 64 navmesh slots cycling through 200 visible tiles, erasing render data on eviction would cause tiles to flicker/disappear.

### Bit budget

| Field | Before | After |
|-------|--------|-------|
| tileBits | 9 | 6 |
| saltBits | 6 | 10 |
| polyBits | 17 | 16 |
| maxTiles | 512 | 64 |
| maxPolys | 131072 | 65536 |

## Part 2: Tile info in Properties panel

### Feature

The Properties panel now shows a "Navmesh Tile" section at the bottom with metadata for the tile under the cursor:

- WoW tile coords and Detour tile coords
- Polygon count, vertex count, detail tris/verts, BV nodes, off-mesh connections, max links
- Walkable height/radius/climb
- Render triangle count, navmesh slot status, render cache status

The section is always visible regardless of node/edge selection. The Properties panel now renders unconditionally (not gated on graph data being loaded).

### Header snapshot caching

Since only 64 tiles fit in the navmesh at a time but up to 200 are visible, most tiles are evicted from the navmesh after triangle extraction. To ensure metadata is available for all visible tiles, `CacheEntry` stores a `TileInfo headerInfo` snapshot taken at extraction time. `GetTileInfo` returns this cached snapshot as the primary source, with "In Navmesh" accurately reflecting current navmesh slot status.

## Files changed

| File | Change |
|------|--------|
| `src/navmesh/tile_loader.cpp` | `kMaxTiles` 512→64, `kSaltBits` 6→10 |
| `src/navmesh/tile_cache.h` | `kMaxNavmeshTiles` 512→64, added `TileInfo` struct, `headerInfo` field in `CacheEntry`, `GetTileInfo()` method |
| `src/navmesh/tile_cache.cpp` | Removed `m_cache.erase` from eviction, snapshot header into `CacheEntry` at extraction, implemented `GetTileInfo()` with cached fallback |
| `src/ui/property_panel.h` | Updated `Render()` signature: added `Canvas&`, `TileCache&` params |
| `src/ui/property_panel.cpp` | Added "Navmesh Tile" section with hovered tile metadata |
| `src/app.cpp` | Updated `PropertyPanel::Render` call site, panel always renders, property panel width no longer conditional |
