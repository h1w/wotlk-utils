# Terrain heightmap rendering + navmesh transparency

**Date:** 2026-02-23
**Task:** TASK-003, Phase 1 + Phase 4

## Summary

Added terrain geometry rendering to the map editor's 3D mode. Parses TrinityCore `.map` files and renders the terrain heightmap as a solid opaque grey mesh underneath the navmesh. The navmesh is now semi-transparent, creating a Recast Demo-like visualization where colored navmesh overlays the solid terrain surface.

## New Files

| File | Description |
|------|-------------|
| `src/data/terrain_loader.h/.cpp` | `.map` file parser — reads V9 (129x129) + V8 (128x128) heightmaps with int16/int8/float decompression, holes data |
| `src/data/terrain_mesh.h/.cpp` | Converts `TerrainTileData` into indexed triangle mesh — 4 triangles per cell (fan from V8 center), per-vertex normals, hole skipping, AABB computation |
| `src/render/terrain_pipeline.h/.cpp` | DX11 shader pipeline — HLSL vertex/pixel shaders with directional lighting, 3 color modes, opaque blend state, depth write ON, back-face cull |
| `src/render/terrain_renderer.h/.cpp` | GPU tile cache with frustum culling, viewport-based streaming (max 4 uploads/frame), LRU eviction (150-tile cap) |

## Modified Files

| File | Change |
|------|--------|
| `src/app.h` | Added `TerrainRenderer m_terrainRenderer` member |
| `src/app.cpp` | Initialize/shutdown terrain renderer; set data path from mmap parent dir; render terrain before navmesh in `RenderFrame3D()`; save/load terrain layer settings |
| `src/render/graph_renderer.h` | Added `showTerrain`, `terrainColorMode` fields to `LayerVisibility` struct |
| `src/ui/layer_panel.cpp` | Added "Terrain" checkbox + "Terrain Color" combo (Solid Grey / Height Gradient / Slope Shading) in 3D mode section |
| `src/data/app_settings.h/.cpp` | Added `showTerrain`, `terrainColorMode` to persisted JSON settings |
| `src/render/navmesh_pipeline_3d.cpp` | Changed depth-stencil: write OFF, comparison LESS_EQUAL (transparent overlay on terrain) |
| `src/render/navmesh_renderer_3d.cpp` | Reduced navmesh fill alpha from 0.9 to 0.45 for see-through effect |
| `map-editor.vcxproj` | Registered 4 new .h + 4 new .cpp files |

## Render Order (3D Mode, updated)

```
1. Clear backbuffer + depth
2. Ground plane (minimap tiles at Z=0, depth write OFF)
3. Terrain heightmap (opaque solid grey, depth write ON)         ← NEW
4. Navmesh (semi-transparent overlay, depth write OFF)           ← MODIFIED
5. Overlays (grid, graph, routes, paths, player marker)
6. ImGui panels
```

## Technical Details

### .map File Parsing

- File path: `{tcDataPath}/maps/{mapId:03d}{tileX:02d}{tileY:02d}.map`
- Header magic: `"MAPS"`, height chunk magic: `"MHGT"`
- Height storage formats: float32 (132KB), uint16 (66KB), uint8 (33KB), or flat (0 bytes)
- Decompression: `height = gridHeight + (value / maxValue) * (gridMaxHeight - gridHeight)`
- Holes: 16x16 grid of uint16 bitmasks, each bit = one sub-cell to skip

### Terrain Mesh Generation

- 33,025 vertices per tile (129x129 V9 corners + 128x128 V8 centers)
- Up to 65,536 triangles per tile (128x128 cells x 4 triangles each)
- Fan tessellation: each cell = 4 triangles fanning from V8 center through V9 corners
- Per-vertex smooth normals computed via face normal accumulation
- Hole cells produce 0 triangles (skipped)

### GPU Pipeline

- **Vertex format**: `float3 POSITION + float3 NORMAL` (24 bytes)
- **Blend**: Opaque (no blending)
- **Depth**: Write ON, test LESS
- **Cull**: Back-face
- **Shaders**: Flat normals via `ddx`/`ddy` (consistent with navmesh), directional lighting with ambient
- **Color modes**: 0 = solid grey, 1 = height gradient (dark→light), 2 = slope shading (flat→steep)
- **Constant buffer**: 112 bytes (viewProj + lightDir + baseColor + heightParams)

### Tile Streaming

- View radius: `cameraDistance * 1.5`
- Nearest-first loading (sorted by distance to camera target)
- Throttled: max 4 GPU uploads per frame
- LRU eviction when cache exceeds 150 tiles (evicts farthest first)
- Global height range recomputed after eviction (for accurate height gradient coloring)

### Navmesh Transparency (Phase 4)

- Navmesh depth-stencil changed: write mask ZERO, comparison LESS_EQUAL
- Fill alpha reduced from 0.9 → 0.45 (terrain visible underneath)
- No z-fighting: navmesh sits naturally above terrain due to Recast agent height offset, combined with LESS_EQUAL test

## Code Review Fixes

- **Removed `row_major`** from HLSL cbuffer — must use default column-major to match navmesh pattern (transposed VP matrix)
- **Added DSV unbind** after terrain render — prevents DX11 state leaking to subsequent passes
- **Recompute global height range** after tile eviction — prevents height gradient from drifting over time
- **Removed unused `enabled` flag** — visibility controlled at call site (consistent with navmesh renderer)
