# Map Editor

Standalone **x64 Windows** desktop application for WoW 3.3.5a (WotLK) navmesh visualization, world graph editing, route planning, and pathfinding testing. Built with ImGui/DX11.

## Features

### Core
- **2D Pan/Zoom Canvas** — north-up map with smooth mouse-wheel zoom and middle-click panning
- **Multi-map support** — switch between all WoW 3.3.5a maps (continents, dungeons, raids)
- **Persistent settings** — window size, last map, directories, layer visibility saved to `app_settings.json`

### Visualization (2D Mode)
- **Navmesh rendering** — loads TrinityCore `.mmtile` files, renders walkable triangles via native DX11 GPU pipeline with per-tile immutable vertex buffers, LOD (detail/base polygons), and viewport-based tile streaming
- **Minimap tile background** — streams BLP minimap textures from WoW MPQ archives via `md5translate.trs`, decoded on background thread, all tiles loaded permanently per map
- **Coordinate grid** — adaptive-step grid (10/100/1000 yd) with tile grid overlay
- **World graph** — renders POI nodes (flight masters, portals, innkeepers, etc.) with color-coded types and directed edges
- **Road graph overlay** — loads `_roads.json` as semi-transparent amber layer beneath the main graph; fully editable in Road Graph mode; dedicated menu item (**Graph > Open Road Graph...**) and layer toggle
- **Route visualization** — numbered waypoints with colored polylines, loop support, direction arrows
- **Path visualization** — Detour pathfinding results rendered as yellow polylines with distance labels
- **Layer panel** — toggle visibility of all render layers independently
- **Legend panel** — color-coded legend for node/edge types

### 3D Mode (Ctrl+2)
- **Orbit camera** — perspective view with mouse orbit, pan, zoom; optional player-follow mode
- **Terrain heightmap** — parses TC `.map` files, renders solid terrain geometry with directional lighting; 3 color modes (Solid Grey, Height Gradient, Slope Shading); background thread loading with LRU eviction
- **Building geometry** — loads M2/WMO models from TC extracted `Buildings/` via `.vmtile` spawn lists; rendered as opaque geometry on top of terrain
- **Ground plane** — minimap tile textures rendered as flat quads at Z=0 underneath terrain
- **Semi-transparent navmesh** — navmesh overlays terrain as colored translucent polygons with optional wireframe edges
- **3D overlays** — coordinate grid, world graph nodes/edges, road graph, routes, path test results drawn as 3D line/circle primitives
- **Player marker** — Ctrl+Click to place, Shift+Click to pathfind; moves along Detour paths at configurable speed (1x-10x); route following support
- **Compass rose** — rotates with camera yaw

### Editing
- **Three editing modes** — Read Only (view only), World Graph (edit POI graph), Road Graph (edit road network); cycled via toolbar button
- **Multi-selection** — Shift+Click to toggle, box selection (drag on empty space), lasso selection (Alt+drag), Ctrl+A to select all
- **Draw Mode (D)** — click to place nodes with auto-connect chain; click existing node to connect; click on edge to auto-split and connect
- **Edge Mode (E)** — click two nodes to create an edge between them
- **Split Edge (S)** — splits nearest edge at cursor position
- **Context menu** — right-click for Delete, Connect Nodes, Split Edge, Merge Nodes, Straighten Path, Add Node, Auto-Connect, Validate Graph
- **Graph operations** — merge nodes (centroid), straighten path (lerp chain), auto-connect endpoints, graph validation (components, dead-ends, duplicates, orphans)
- **Bulk property editing** — multi-select: change Type/Faction for all selected nodes, Edge Type/Bidirectional for all selected edges
- **Route editor** — create/edit/delete routes with waypoint placement by clicking on canvas
- **Undo/Redo** — snapshot-based history (Ctrl+Z / Ctrl+Shift+Z), up to 100 levels; independent stacks for world graph and road graph
- **Property panel** — inspector for selected nodes, edges, and waypoints; multi-selection summary
- **Terrain height sampling** — **Tools > Assign Terrain Heights** batch-assigns real Z values to graph nodes; new nodes auto-sample height on creation
- **Path testing** — click two points to visualize Detour navmesh pathfinding between them
- **Help panel (F1)** — comprehensive keyboard shortcuts and feature reference

### Data Formats
- **Navmesh** — TrinityCore `.mmtile` files (Detour format with MMAP header)
- **Terrain** — TrinityCore `.map` files (V9/V8 heightmap grids, hole bitmask)
- **Buildings** — TC extracted `.vmtile` (spawn lists) + `Buildings/*.m2` and `*.wmo` (model geometry)
- **World graph** — `world_graph.json` (nodes + edges with types, factions, costs)
- **Routes** — `routes.json` (named routes with waypoints, actions, colors)
- **Minimap tiles** — WoW MPQ BLP textures resolved via `md5translate.trs`
- **Map definitions** — `Map.dbc` from MPQ for map ID/name lookups

### UI
- **Menu bar** — File (open/save graph+routes), Edit (undo/redo/delete), Graph (road graph save), View (panels/legend/help), Map (selector)
- **Status bar** — cursor world coordinates, tile coordinates, zoom level, current map name
- **Log window** — glog-integrated log viewer with severity filters, text search, auto-scroll, docked bottom-left
- **File dialogs** — native Win32 open/save dialogs

## Quick Start

See [BUILD.md](BUILD.md) for build instructions.

1. Build `map-editor.vcxproj` (Release|x64)
2. Run `map-editor.exe`
3. File > Set mmaps Directory > select your TrinityCore `mmaps/` folder
4. File > Set WoW Directory > select your WoW 3.3.5a installation (for minimap tiles)
5. File > Open World Graph > select `world_graph.json`
6. Use the Map menu or dropdown to switch maps

## Documentation Index

| Document | Description |
|----------|-------------|
| [README.md](README.md) | This file — project overview |
| [ARCHITECTURE.md](ARCHITECTURE.md) | Code architecture, modules, data flow |
| [BUILD.md](BUILD.md) | Build instructions, dependencies, configuration |
| [COORDINATE_SYSTEM.md](COORDINATE_SYSTEM.md) | WoW coordinate system, tile mapping, transforms |
| [tasks/TASK_001.md](tasks/TASK_001.md) | Initial 2D editor implementation |
| [tasks/TASK_002.md](tasks/TASK_002.md) | 3D mode, camera, overlays, player simulation |
| [tasks/TASK_003.md](tasks/TASK_003.md) | Terrain heightmap + building geometry rendering |
| [ROAD_GRAPH_USAGE.md](ROAD_GRAPH_USAGE.md) | Road extraction output usage guide |
| [changelog/](changelog/) | Per-feature/fix changelogs |
