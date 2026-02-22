# TASK-001: Map Editor — Full Implementation

**Status**: DONE
**Created**: 2025
**Last Updated**: 2026-02-21

---

## Overview

Standalone **x64 Windows application** — visual navmesh map editor for WoW 3.3.5a with ImGui/DX11. Allows viewing navmesh, editing world graph (`world_graph.json`), creating routes, and testing Detour pathfinding.

**Location**: `map-editor/` in the `wotlk-utils` repository.

---

## Phase Status Summary

| Phase | Description | Status |
|-------|-------------|--------|
| Phase 1 | Skeleton App + Canvas + Tile Grid | DONE |
| Phase 2 | Navmesh Polygon Rendering | DONE |
| Phase 3 | World Graph Display + Editing | DONE |
| Phase 4 | Custom Route Editor | DONE |
| Phase 5 | Pathfinding Testing | DONE |
| Post | MPQ Integration + Minimap Tiles | DONE |
| Post | Log Window | DONE |
| Post | Undo/Redo | DONE |
| Post | Settings Persistence | DONE |
| Post | Background Mode Selector | DONE |

---

## Phase 1: Skeleton App + Canvas + Tile Grid — DONE

### Deliverables
- [x] DX11/ImGui window (`main.cpp`, `app.h/.cpp`)
- [x] 2D pan/zoom canvas with WoW coordinate transforms (`canvas/canvas.h/.cpp`)
- [x] Map definitions with 72 WoW maps (`data/map_defs.h`)
- [x] Tile index scanner for `.mmtile` files (`data/tile_index.h/.cpp`)
- [x] Adaptive coordinate grid + tile grid renderer (`render/grid_renderer.h/.cpp`)
- [x] Status bar with cursor coords, tile position, zoom (`ui/status_bar.h/.cpp`)

### Files Created
```
src/main.cpp
src/app.h, src/app.cpp
src/canvas/canvas.h, src/canvas/canvas.cpp
src/data/map_defs.h
src/data/tile_index.h, src/data/tile_index.cpp
src/render/grid_renderer.h, src/render/grid_renderer.cpp
src/ui/status_bar.h, src/ui/status_bar.cpp
```

---

## Phase 2: Navmesh Polygon Rendering — DONE

### Deliverables
- [x] mmtile loader with TC 16-byte→12-byte dtLink repack (`navmesh/tile_loader.h/.cpp`)
- [x] Viewport-based tile streaming with LRU cache, 200 tiles (`navmesh/tile_cache.h/.cpp`)
- [x] Triangle extraction from dtNavMesh detail meshes
- [x] Navmesh polygon rendering via ImDrawList (`render/navmesh_renderer.h/.cpp`)
- [x] 32-bit dtPolyRef fix: `maxTiles=512`, `maxPolys=8192`

### Files Created
```
src/navmesh/tile_loader.h, src/navmesh/tile_loader.cpp
src/navmesh/tile_cache.h, src/navmesh/tile_cache.cpp
src/render/navmesh_renderer.h, src/render/navmesh_renderer.cpp
```

---

## Phase 3: World Graph Display + Editing — DONE

### Deliverables
- [x] World graph JSON load/save with full CRUD (`data/world_graph_data.h/.cpp`)
- [x] Graph renderer: color-coded nodes by type, directed edges (`render/graph_renderer.h/.cpp`)
- [x] Selection state machine for nodes/edges/waypoints (`editor/selection.h/.cpp`)
- [x] Graph editor: add/remove/drag nodes, create/delete edges (`editor/graph_editor.h/.cpp`)
- [x] Property panel: inspector for selected objects (`ui/property_panel.h/.cpp`)
- [x] Layer panel: visibility toggles (`ui/layer_panel.h/.cpp`)
- [x] Main menu: File/Edit/View/Map with keyboard shortcuts (`ui/main_menu.h/.cpp`)
- [x] Native Win32 file dialogs for open/save

### Files Created
```
src/data/world_graph_data.h, src/data/world_graph_data.cpp
src/render/graph_renderer.h, src/render/graph_renderer.cpp
src/editor/selection.h, src/editor/selection.cpp
src/editor/graph_editor.h, src/editor/graph_editor.cpp
src/ui/property_panel.h, src/ui/property_panel.cpp
src/ui/layer_panel.h, src/ui/layer_panel.cpp
src/ui/main_menu.h, src/ui/main_menu.cpp
```

---

## Phase 4: Custom Route Editor — DONE

### Deliverables
- [x] Route data JSON load/save with CRUD (`data/route_data.h/.cpp`)
- [x] Route editor: waypoint placement, drag, delete (`editor/route_editor.h/.cpp`)
- [x] Route renderer: numbered waypoints, colored polylines, direction arrows (`render/route_renderer.h/.cpp`)
- [x] Loop support with dashed closing line

### Files Created
```
src/data/route_data.h, src/data/route_data.cpp
src/editor/route_editor.h, src/editor/route_editor.cpp
src/render/route_renderer.h, src/render/route_renderer.cpp
```

---

## Phase 5: Pathfinding Testing — DONE

### Deliverables
- [x] Detour pathfinding wrapper (`navmesh/pathfinder.h/.cpp`)
- [x] Path renderer: yellow polyline, A/B markers, distance label (`render/path_renderer.h/.cpp`)
- [x] Test path mode: click A, click B, auto-pathfind

### Files Created
```
src/navmesh/pathfinder.h, src/navmesh/pathfinder.cpp
src/render/path_renderer.h, src/render/path_renderer.cpp
```

---

## Post-Phase Work — DONE

### MPQ Integration + Minimap Tile Background
- [x] StormLib MPQ archive set with priority ordering (`mpq/mpq_archive.h/.cpp`)
- [x] BLP2 texture decoder — DXT1/3/5, paletted, raw BGRA (`mpq/blp_decoder.h/.cpp`)
- [x] DBC reader for Map.dbc (`mpq/dbc_reader.h/.cpp`)
- [x] Minimap tile cache with background thread decode (`render/minimap_cache.h/.cpp`)
- [x] `md5translate.trs` parsing for BLP path resolution
- [x] Correct tile lookup key swap (WoW client convention: `map{ty}_{tx}`)
- [x] Correct UV vertical flip for BLP orientation
- [x] All tiles for selected map loaded permanently (no LRU eviction, no viewport limit)
- [x] Background mode selector: None / MinimapTiles / ZoneWorldMaps(TODO) / ADTHeightmap(TODO)

### Log Window
- [x] glog LogSink integration with ImGui window (`ui/log_window.h/.cpp`)
- [x] Severity color coding (INFO=green, WARN=yellow, ERROR=red)
- [x] Severity filters, text search filter
- [x] Auto-scroll (always tracks newest line when enabled)
- [x] Docked to bottom-left corner, above status bar
- [x] File logging to `logs/` directory

### Undo/Redo System
- [x] Snapshot-based undo/redo (`editor/undo_redo.h/.cpp`)
- [x] Captures full graph + route state before each mutation
- [x] Up to 100 undo levels
- [x] Ctrl+Z / Ctrl+Y keyboard shortcuts

### Settings Persistence
- [x] JSON settings file (`data/app_settings.h/.cpp`)
- [x] Persists: directories, last map, layer visibility, background mode/opacity, window size

### Legacy / Unused
- [ ] World map loader (`mpq/world_map_loader.h/.cpp`) — zone-level artistic maps, not coordinate-aligned
- [ ] Map background static image (`render/map_background.h/.cpp`) — replaced by minimap tiles

---

## Known Issues / Future Work

### Background Modes (stubs)
- **ZoneWorldMaps** — enum value exists, not implemented. Would load `Interface\WorldMap\{Zone}\*.blp`
- **ADTHeightmap** — enum value exists, not implemented. Would generate height-colored tiles from ADT data

### Performance
- Drawing 500+ minimap tile quads via ImDrawList may impact frame time at extreme zoom-out — not observed in practice but worth monitoring

### Editing
- Node Z coordinate must be set manually (no navmesh height raycast on placement)
- No multi-select for batch operations

---

## Critical Constants

```cpp
static constexpr float TILE_SIZE = 533.33333f;  // 1 ADT tile in yards
// tileX = 31 - floor(wowX / TILE_SIZE)   [corrected: was 32, see changelog 2026-02-22]
// tileY = 31 - floor(wowY / TILE_SIZE)

// Detour <-> WoW coordinate mapping:
// Detour[0] = WoW Y, Detour[1] = WoW Z, Detour[2] = WoW X

// dtPolyRef 32-bit budget:
// maxTiles=64 (tileBits=6), maxPolys=65536 (polyBits=16), saltBits=10
```

---

## Original Task File

This task was migrated from `map-editor/MAP_EDITOR_TASK.md` which contained the original detailed implementation specification including code snippets, data structure definitions, and per-phase checklists. The original file is preserved for reference.
