# Fix 3D rendering (terrain, buildings, ground plane) + reduce log spam

**Date:** 2026-02-23

## Summary

Fixed three bugs preventing terrain, buildings, and the ground plane from rendering in 3D mode. Also removed verbose per-asset logging that was flooding the log with thousands of lines during normal operation.

## Bug Fixes

### 1. TC data path not found (terrain + buildings not loading)

**Root cause:** `m_mmapDir` has a trailing backslash (e.g. `Z:\...\mmaps\`). `std::filesystem::path::parent_path()` on a path with a trailing separator merely strips the separator instead of navigating up a directory. So the parent resolved to `mmaps` itself rather than `wotlk\`, and the probe for `maps/` and `trinitycore_data/maps/` checked the wrong directory.

**Fix:** Strip the trailing separator before calling `parent_path()` using `filename().empty()` detection. Also added auto-detection for two directory layouts: direct sibling (`{parent}/maps/`) and nested (`{parent}/trinitycore_data/maps/`).

### 2. Ground plane not rendering in 3D mode

**Root cause:** `MinimapTileCache::UpdateViewport()` was only called when `m_bgMode == MinimapTiles`, but saved settings had `bg_mode: 0` (ZoneWorldMaps). The 3D ground plane uses the same minimap cache via `ForEachCachedTile()`, which iterated over an empty cache.

**Fix:** Expanded the update condition to also trigger when `viewMode == Mode3D && showGroundPlane`.

### 3. Excessive log spam

Removed per-asset `LOG(INFO)` calls that produced thousands of lines during normal tile loading:
- `BuildingLoader` logged every individual .m2/.wmo model (hundreds per tile)
- `MinimapCache` logged every 50th decoded tile
- `VMapTileLoader` logged every .vmtile file

The higher-level summary logs remain (e.g. `[BuildingRenderer] Tile (31,31): 687 models, 61910 verts`).

## Modified Files

| File | Change |
|------|--------|
| `src/app.cpp` | Fix `parent_path()` trailing-separator bug (2 locations: Initialize + file dialog); expand minimap cache update condition for 3D mode |
| `src/data/building_loader.cpp` | Remove per-model `LOG(INFO)` |
| `src/data/vmap_tile_loader.cpp` | Remove per-vmtile `LOG(INFO)` |
| `src/render/minimap_cache.cpp` | Remove per-tile `LOG(INFO)` |
