# Building geometry rendering (VMAP models + vmtile placement)

**Date:** 2026-02-23
**Task:** TASK-003, Phase 2 + Phase 3

## Summary

Added building and object geometry rendering to the map editor's 3D mode. Parses TrinityCore's pre-extracted collision meshes from the `Buildings/` directory (`.wmo` and `.m2` files in VMAP048 format), reads spawn placement data from `.vmtile` files, transforms model vertices to WoW world coordinates, and renders the assembled geometry as solid opaque meshes alongside the terrain.

## New Files

| File | Description |
|------|-------------|
| `src/data/building_loader.h/.cpp` | VMAP048 model parser — reads `GRP`/`INDX`/`VERT` chunks from Buildings/*.wmo/*.m2, merges multi-group geometry, caches loaded models |
| `src/data/vmap_tile_loader.h/.cpp` | `.vmtile` placement parser — reads `VMAP_4.8` format with per-spawn position, rotation (ZYX Euler), scale, and model name references |
| `src/render/building_renderer.h/.cpp` | GPU tile cache for buildings — loads vmtile spawns, transforms model vertices, merges per tile into single VB/IB, renders with terrain pipeline |

## Modified Files

| File | Change |
|------|--------|
| `src/app.h` | Added `#include "render/building_renderer.h"` and `BuildingRenderer m_buildingRenderer` member |
| `src/app.cpp` | Initialize/shutdown building renderer; set data path from mmap parent dir (both init-from-settings and File>Open); render buildings between terrain and navmesh in `RenderFrame3D()`; save/load building layer settings |
| `src/render/graph_renderer.h` | Added `bool showBuildings = true` to `LayerVisibility` struct |
| `src/ui/layer_panel.cpp` | Added "Buildings" checkbox in 3D mode section |
| `src/data/app_settings.h/.cpp` | Added `showBuildings` to persisted JSON settings |
| `map-editor.vcxproj` | Registered 3 new .h + 3 new .cpp files |

## Render Order (3D Mode, updated)

```
1. Clear backbuffer + depth
2. Ground plane (minimap tiles at Z=0, depth write OFF)
3. Terrain heightmap (opaque solid grey, depth write ON)
4. Building geometry (opaque solid grey, depth write ON)      <- NEW
5. Navmesh (semi-transparent overlay, depth write OFF)
6. Overlays (grid, graph, routes, paths, player marker)
7. ImGui panels
```

## Technical Details

### VMAP048 Model Format (Buildings/*.wmo, *.m2)

- Magic: `VMAP048\0` (8 bytes)
- Root header: `nVertices(4) + nGroups(4) + rootWMOID(4)`
- Per group: `mogpFlags(4) + groupWMOID(4) + bbox(24) + liquidType(4)` + `GRP` chunk + `INDX` chunk (uint16 indices) + `VERT` chunk (float3 vertices) + optional `LIQU` chunk
- Multi-group models (e.g. Stormwind.wmo with 300K+ triangles) merged into single mesh with running vertex offset
- Models cached in `std::unordered_map<string, BuildingMesh>` to avoid re-reading for shared models across tiles

### vmtile Placement Format (vmaps/*.vmtile)

- **File naming**: `MMM_YY_XX.vmtile` — tileY comes BEFORE tileX
- Magic: `VMAP_4.8` (8 bytes), then `nSpawns(4)`
- Per spawn: `flags(4) + adtId(2) + ID(4) + iPos(12) + iRot(12) + iScale(4) + [bounds(24) if flags&0x04] + nameLen(4) + name(nameLen) + nodeIdx(4)`
- Spawn flags: `MOD_M2=0x01`, `MOD_WORLDSPAWN=0x02`, `MOD_HAS_BOUND=0x04`

### Coordinate Transform Pipeline

```
Model-local vertex
  -> Scale (spawn.scale)
  -> Rotate (ZYX Euler: Rz(yaw) * Ry(pitch) * Rx(roll))
  -> Translate (add to spawn position in internal VMAP coords)
  -> Convert to WoW coords: wowX = MID - internalX, wowY = MID - internalY, wowZ = internalZ
```

Where `MID = 0.5 * 64 * 533.3333 = 17066.666`

### Rendering

- Reuses `TerrainPipeline` (same vertex format, shaders, render states)
- Building base color slightly different from terrain (0.50/0.48/0.45 vs 0.45/0.45/0.48) for visual distinction
- Always uses solid grey color mode (no height gradient for buildings)
- Normals computed by pixel shader via `ddx`/`ddy` (flat normals, since building vertex normals are placeholders)
- Frustum culling per tile using AABB
- Empty `TileBuildings` entries inserted for tiles with no vmtile to avoid re-trying every frame

### Tile Streaming

- View radius: `cameraDistance * 1.2` (smaller than terrain since buildings are less important at distance)
- Max 2 GPU uploads per frame (buildings are heavier than terrain)
- LRU eviction at 100 tiles
- All buildings in one tile merged into single draw call
