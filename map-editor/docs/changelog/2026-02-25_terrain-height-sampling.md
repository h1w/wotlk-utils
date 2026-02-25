# Terrain Height Sampling + Road Graph 3D Rendering

**Date:** 2026-02-25

## Summary

Added terrain height sampling for graph nodes. Road graph nodes (extracted from 2D alpha maps with `z=0`) can now be assigned real terrain heights via a new Tools menu action. New nodes created in the editor automatically receive terrain-sampled heights. Road graph is now visible in 3D mode.

## Changes

### New: TerrainHeightSampler (`src/data/terrain_height_sampler.h/.cpp`)

- **`TerrainHeightSampler`** class — CPU-side terrain height lookup from TC `.map` files
- **`SampleHeight(mapId, wowX, wowY)`** — returns `std::optional<float>` terrain Z at any WoW coordinate
- **`AssignHeights(mapId, nodes)`** — batch-assigns terrain heights to all graph nodes on the given map; returns count of updated nodes
- **LRU tile cache** — caches up to 64 `TerrainTileData` tiles (~8.5 MB max) to avoid redundant disk I/O
- **Barycentric interpolation** — matches `terrain_mesh.cpp` triangle-fan layout (4 triangles per cell from V8 center to V9 corners); determines which triangle the query point falls in via diagonal tests, then interpolates height within that triangle

### Edit: Graph Editor (`src/editor/graph_editor.h/.cpp`)

- **`SetHeightSampler(TerrainHeightSampler*)`** — connects the editor to the height sampler
- **Auto height assignment** — all 6 node creation points now sample terrain height automatically:
  - Context menu "Add Node Here"
  - Context menu "Split Edge"
  - S-key split edge at cursor
  - Draw mode split-on-edge
  - Draw mode new node (D key)
  - Double-click add node
- Height sampling is optional (null-safe) — falls back to `z=0` or midpoint average if sampler unavailable or tile not loaded

### Edit: App (`src/app.h/.cpp`)

- **Initialization** — `m_heightSampler.SetDataPath(tcDataPath)` called alongside terrain/building renderers (both startup restore and runtime directory selection)
- **`m_graphEditor.SetHeightSampler(&m_heightSampler)`** — wired up during `Initialize()`
- **Tools menu** — new "Assign Terrain Heights" item:
  - Applies to the currently active graph (world or road)
  - Takes an undo snapshot before modification
  - Calls `AssignHeights()` for all nodes on the current map
  - Marks graph as dirty (triggers save prompt)
  - Disabled when no graph is loaded
- **3D ReadOnly rendering** — road graph now renders in 3D mode alongside world graph:
  - Previously: only world graph rendered in ReadOnly 3D mode
  - Now: both `showRoadGraph` and `showWorldGraph` layers are checked and rendered independently
  - Matches the 2D rendering behavior where both graphs are visible in ReadOnly mode

### Edit: Project (`map-editor.vcxproj`)

- Added `terrain_height_sampler.h` and `terrain_height_sampler.cpp` to ClInclude/ClCompile groups

## Height Sampling Algorithm

```
1. Tile lookup:
   tileX = 31 - floor(wowX / 533.33)
   tileY = 31 - floor(wowY / 533.33)

2. Local cell:
   localX = tileOriginX - wowX    (tileOriginX = (32 - tileX) * 533.33)
   localY = tileOriginY - wowY
   cellRow = floor(localX / CELL_SIZE), clamped [0, 127]
   cellCol = floor(localY / CELL_SIZE), clamped [0, 127]

3. Interpolation:
   V9 corners (TL, TR, BL, BR) + V8 center → 4-triangle fan
   Two diagonals divide the cell into top/bottom/left/right triangles
   Barycentric interpolation within the matched triangle
```

## Modified Files

| File | Action | Changes |
|------|--------|---------|
| `src/data/terrain_height_sampler.h` | NEW | TerrainHeightSampler class declaration |
| `src/data/terrain_height_sampler.cpp` | NEW | LRU cache, interpolation, batch assign |
| `src/editor/graph_editor.h` | EDIT | `SetHeightSampler()`, `m_heightSampler` member |
| `src/editor/graph_editor.cpp` | EDIT | 6 node-creation sites auto-sample height |
| `src/app.h` | EDIT | `#include`, `m_heightSampler` member |
| `src/app.cpp` | EDIT | Init wiring, Tools menu item, 3D road graph rendering |
| `map-editor.vcxproj` | EDIT | Added new source files |

## Usage

1. Load a road graph: **Graph > Open Road Graph...** > select `Azeroth_roads.json`
2. Assign heights: **Tools > Assign Terrain Heights** (requires mmaps directory set)
3. View in 3D: **Ctrl+2** to switch to 3D mode — road graph nodes now sit on terrain surface
4. New nodes automatically get terrain heights when created via double-click, draw mode, or context menu
5. Undo with **Ctrl+Z** if heights look wrong
