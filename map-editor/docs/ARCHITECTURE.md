# Architecture

## Overview

The map editor is a single-process x64 Windows desktop application using ImGui for UI and Direct3D 11 for rendering. All subsystems are owned by the `App` class and communicate through shared data structures.

**Namespace**: `mapedit`

## Source Tree

```
map-editor/src/
    main.cpp                    WinMain entry, DX11 bootstrap
    app.h / app.cpp             App class — owns all subsystems, main loop

    canvas/
        canvas.h / canvas.cpp   2D pan/zoom camera, WoW<->screen transforms

    data/
        map_defs.h              Static map ID/name table (72 maps)
        tile_index.h / .cpp     Scan mmaps dir -> tile existence bitset
        world_graph_data.h/.cpp World graph JSON load/save/CRUD
        route_data.h / .cpp     Route JSON load/save/CRUD
        app_settings.h / .cpp   Persistent settings (JSON file)
        terrain_loader.h / .cpp TC .map file parser (V9/V8 heightmaps, holes)
        terrain_mesh.h / .cpp   Heightmap -> indexed triangle mesh + normals
        terrain_height_sampler.h/.cpp CPU-side terrain height lookup (LRU-cached, barycentric)
        vmap_tile_loader.h/.cpp TC .vmtile file parser (M2/WMO spawn list)
        building_loader.h / .cpp M2/WMO collision geometry loader (TC VMAP048 format)
        wmo_visual_loader.h/.cpp WMO visual geometry from MPQ (full walls, roofs, interiors)
        wmo_portal_loader.h/.cpp WMO portal graph from MPQ (indoor/outdoor culling)

    navmesh/
        tile_loader.h / .cpp    Load .mmtile -> dtNavMesh, extract triangles
        tile_cache.h / .cpp     Viewport-based navmesh tile streaming + LRU
        pathfinder.h / .cpp     Detour findPath wrapper

    mpq/
        mpq_archive.h / .cpp    StormLib MPQ archive set (priority-ordered)
        blp_decoder.h / .cpp    BLP2 texture decoder (DXT1/3/5, paletted, raw)
        dbc_reader.h / .cpp     DBC database file reader
        world_map_loader.h/.cpp Zone world map BLP loading (unused legacy)

    editor/
        selection.h             MultiSelection (unordered_set-based multi-node/edge)
        graph_editor.h / .cpp   Full graph editor: draw/edge/split modes, context menu, drag,
                                lasso/box selection, merge, straighten, auto-connect, validate.
                                EditorProjection abstraction enables same code for 2D and 3D.
                                3D node placement uses ray-terrain intersection (ray marching
                                + binary search against TerrainHeightSampler).
        graph_validator.h/.cpp  Graph validation (disconnected components, gaps, dead-ends, etc.)
        route_editor.h / .cpp   Route waypoint placement + drag
        undo_redo.h / .cpp      Snapshot-based undo/redo (100 levels, separate stacks per graph)

    render/
        grid_renderer.h / .cpp      Coordinate + tile grid
        navmesh_pipeline.h / .cpp   DX11 GPU pipeline (shaders, state, CB)
        navmesh_renderer.h / .cpp   GPU navmesh rendering via DX11 callback
        graph_renderer.h / .cpp     World graph nodes + edges
        route_renderer.h / .cpp     Route waypoints + lines
        path_renderer.h / .cpp      Pathfinding result
        map_background.h / .cpp     Static background image (legacy)
        minimap_cache.h / .cpp      Minimap tile streaming from MPQ
        navmesh_pipeline_3d.h/.cpp  DX11 3D pipeline (perspective, lighting)
        navmesh_renderer_3d.h/.cpp  3D navmesh rendering (direct DX11 calls)
        primitives_3d.h / .cpp      3D line/sphere drawing utility
        graph_renderer_3d.h / .cpp  3D graph nodes + edges
        route_renderer_3d.h / .cpp  3D route waypoints + lines
        path_renderer_3d.h / .cpp   3D pathfinding result
        grid_renderer_3d.h / .cpp   3D coordinate grid at Z=0
        ground_plane_3d.h / .cpp    Minimap texture ground plane
        terrain_pipeline.h / .cpp   DX11 terrain shader pipeline (lighting, color modes)
        terrain_renderer.h / .cpp   Terrain GPU tile cache + background loading
        building_renderer.h / .cpp  Building GPU tile cache + background loading
        frame_profiler.h            Per-layer CPU profiler (header-only, QPC timing)

    ui/
        main_menu.h / .cpp          File/Edit/View/Map menu bar
        property_panel.h / .cpp     Inspector for selected objects
        layer_panel.h / .cpp        Layer visibility + background mode
        issue_panel.h / .cpp        Graph issues panel (validation overlay, gap filter, type toggles)
        status_bar.h / .cpp         Cursor coords, zoom, map name
        log_window.h / .cpp         glog-integrated log viewer
        compass.h / .cpp            Compass rose overlay (2D static / 3D rotating)

    camera/
        camera3d.h / .cpp           3D orbit camera (perspective, Z-up)

    simulation/
        player_marker.h / .cpp      Player position + movement along paths
```

**Total**: ~90 files (45 .h + 45 .cpp)

## Data Flow

```
                    ┌─────────────┐
                    │  App::Run() │  Main loop
                    └──────┬──────┘
                           │
          ┌────────────────┼────────────────┐
          ▼                ▼                ▼
    ┌──────────┐   ┌──────────────┐  ┌───────────┐
    │  Canvas   │   │   Editors    │  │    UI      │
    │ pan/zoom  │   │ graph/route  │  │ panels/bar │
    └─────┬────┘   └──────┬───────┘  └───────────┘
          │                │
          ▼                ▼
    ┌──────────┐   ┌──────────────┐
    │ Renderers │   │   Data       │
    │ draw cmds │   │ graph/routes │
    └──────────┘   └──────────────┘
          │
    ┌─────┴───────────────────┐
    ▼                         ▼
┌──────────┐          ┌──────────────┐
│ TileCache│          │MinimapTileCache│
│ navmesh  │          │ MPQ BLP tiles  │
└──────────┘          └──────────────┘
```

## Key Subsystems

### Canvas (`canvas/`)

Manages the 2D viewport — camera position in WoW coordinates, zoom level (pixels/yard), and coordinate transforms. North-up orientation: screen-right = WoW east (-Y), screen-down = WoW south (-X).

### Navmesh Streaming (`navmesh/`)

**TileCache** loads `.mmtile` files on demand based on viewport, with LRU eviction (configurable, default 200 tiles). Two-layer cache: rendering cache (extracted triangles, survives navmesh eviction) + Detour navmesh cache (max 64 tiles for pathfinding). Tiles loaded nearest-first with disk I/O throttling (8 tiles/frame).

**TileLoader** handles the TC mmtile format including 16-byte→12-byte dtLink repack for stock Detour compatibility. Uses overridden `maxTiles=64` / `maxPolys=65536` to fit 32-bit dtPolyRef bit budget (`tileBits(6) + saltBits(10) + polyBits(16) = 32`).

Tile coordinate mapping: `tileX = 31 - floor(wowX / 533.33)`. See [COORDINATE_SYSTEM.md](COORDINATE_SYSTEM.md) for full reference.

### Minimap Tile Streaming (`render/minimap_cache.cpp`)

Loads BLP minimap textures from WoW MPQ archives. Architecture:

1. **Initialize** — parse `Map.dbc` (mapId→InternalName) + `md5translate.trs` (tile key→BLP hash path)
2. **SetMap** — clear cache, reset state
3. **UpdateViewport** — on first frame after map change, scan all 64x64 tile positions, read raw BLP bytes from MPQ on main thread, push to worker
4. **WorkerLoop** (background thread) — decode BLP → BGRA pixels
5. **ProcessCompletedTiles** (main thread) — create D3D11 textures from decoded results
6. **Render** — draw all cached tiles as textured quads via ImGui

Thread safety: MPQ reads on main thread (StormLib not thread-safe), BLP decode on worker, D3D11 texture creation on main thread.

### World Graph (`data/world_graph_data.cpp`)

Full CRUD for the navigation graph: nodes (POIs with type, faction, coordinates) and edges (connections with type, cost, directionality). Dirty-flag tracking for unsaved changes.

The app holds two `WorldGraphData` instances: `m_graphData` (world/POI graph) and `m_roadGraphData` (road network). Three editing modes control which is active: **Read Only** (view both, edit neither), **World Graph** (edit world graph, road as overlay), **Road Graph** (edit road graph, world as overlay). Each graph has its own `UndoRedo` stack. The road graph loads via **Graph > Open Road Graph...** and saves via **Graph > Save Road Graph**.

### Undo/Redo (`editor/undo_redo.cpp`)

Snapshot-based: captures full graph+route state before each mutation. Supports up to 100 undo levels. Both undo and redo stacks maintained. The app has two separate `UndoRedo` instances (`m_undoRedo` for world graph, `m_roadUndoRedo` for road graph); the active stack is selected based on the current editing mode (`ActiveGraph`).

### MPQ Reading (`mpq/`)

**MpqArchiveSet** opens all WoW Data MPQs in priority order (patch-3 > patch-2 > patch > lichking > expansion > common). Reads files from highest-priority archive first. Also scans locale subdirectories (enUS, ruRU, etc.).

### Terrain Rendering (`data/terrain_loader.cpp`, `render/terrain_renderer.cpp`)

Loads TrinityCore `.map` files containing heightmap data (V9 129x129 + V8 128x128 grids per tile) and renders solid terrain geometry in 3D mode.

**Data pipeline**: `TerrainLoader` parses `.map` files (int16/int8/float decompression, hole bitmask) → `TerrainMesh` converts to indexed triangle mesh (4 triangles per cell, fan from V8 center, per-vertex normals) → `TerrainRenderer` manages GPU tile cache with background thread loading, frustum culling, LRU eviction (150-tile cap), and max 4 uploads/frame.

**Rendering**: Two pipelines — `TerrainPipeline` (procedural color modes) and `TerrainTexturePipeline` (textured terrain). Both share the same `TerrainCB` constant buffer layout (128 bytes) and `TerrainVertexGpu` format (36 bytes: position + normal + UV + slotIndex).

- **Procedural** (`colorMode` 0/1/2): Solid Grey, Height Gradient, Slope Shading. Single CB update for entire batch.
- **Textured** (`colorMode` 3): Samples from a 64-slot `Texture2DArray` (BC1_UNORM, 1024×1024, 11 mips). Texture array slot index is baked into each vertex at upload time (`TerrainVertex.slotIndex`), enabling single CB update for entire textured batch. Anisotropic 8× filtering.

Opaque blend, depth write ON, back-face cull. Renders before navmesh so navmesh overlays as semi-transparent. **Smooth toggle** controls two shader-level behaviors: (1) normal mode — smooth per-vertex normals vs flat ddx/ddy normals (`heightParams.w`: -1.0=smooth, -2.0=flat); (2) slope-dependent Z offset via `baseColor.a` — pushes terrain below navmesh on steep slopes (flat: -1 unit, vertical: -6 units) to prevent grey terrain bumps poking through the navmesh overlay.

### Terrain Height Sampling (`data/terrain_height_sampler.cpp`)

CPU-side point-query for terrain height at any WoW coordinate. Uses `TerrainLoader` to read `.map` files with an LRU tile cache (64 tiles, ~8.5 MB). Barycentric interpolation in the triangle-fan grid (4 triangles per cell from V8 center to V9 corners) matches the GPU mesh exactly.

Used by `GraphEditor` to auto-assign Z when creating/splitting nodes, and by the **Tools > Assign Terrain Heights** menu action to batch-update all nodes in the active graph. This solves the road graph `z=0` problem — nodes extracted from 2D alpha-map texture analysis have no elevation data and need height sampling from terrain data to render correctly in 3D. Also used by `EditorProjection3D::ScreenToWorldOnTerrain` for ray-terrain intersection (ray marching + binary search) to accurately place nodes on sloped terrain in 3D mode.

### Building Rendering (`render/building_renderer.cpp`)

Two-tier data pipeline with visual-first loading:

1. **VMapTileLoader** parses `.vmtile` files from `vmaps/` — each contains a list of M2/WMO model spawns with positions, rotations, and model filenames
2. **WmoVisualLoader** (PRIMARY) — loads full visual geometry from WMO group files in MPQ archives. Complete walls, roofs, arches, staircases, interiors. Uses 2-tier MPQ path resolution (underscore-to-backslash + case-insensitive basename index)
3. **BuildingLoader** (FALLBACK) — loads TC collision geometry from `Buildings/` directory when MPQ visual data unavailable. Used for M2 props and WMOs not found in MPQ
4. **WmoPortalLoader** — loads portal graph from WMO root files for indoor/outdoor visibility culling

**BuildingRenderer** manages a GPU tile cache with background thread loading. Each tile aggregates all spawns' transformed geometry into a single vertex/index buffer. Opaque, depth write ON, renders after terrain and before navmesh.

**Portal culling**: When camera enters a WMO group bounding box, BFS traversal through portal graph determines which groups are visible. From outside, all groups are rendered (depth buffer handles occlusion).

**TC data path auto-detection**: probes `{mmaps_parent}/maps/` (sibling layout) and `{mmaps_parent}/trinitycore_data/maps/` (nested layout) to locate terrain `.map` files, `vmaps/` tiles, and `Buildings/` models.

### 3D Camera (`camera/camera3d.cpp`)

Orbit camera with perspective projection. Parameters: target position (WoW XYZ), yaw, pitch, distance. Supports mouse orbit (right-drag), pan (middle-drag), zoom (scroll). Optional follow mode tracks the player marker. Uses `XMMatrixScaling(-1,1,1)` X-flip so east (WoW -Y) appears screen-right.

### Player Simulation (`simulation/player_marker.cpp`)

Virtual player marker for 3D mode. Supports Ctrl+Click teleport, Shift+Click pathfind-to-destination (via Detour), and route following. Moves along waypoints at configurable speed (1x-10x). Camera follow mode tracks the player.

### Settings Persistence (`data/app_settings.cpp`)

JSON file (`map_editor_settings.json`) stores: directories, last map, layer visibility, background mode/opacity, window dimensions, terrain/building layer toggles, issue panel state (graph mode, view mode, panel open/closed, gap distance slider, overlay toggle, type filter toggles). Loaded on startup, saved on exit.

### Exit Confirmation (`app.cpp`)

All exit paths (window close button, Alt+F4, File > Exit) go through `RequestQuit()`. If any data source (`m_graphData`, `m_roadGraphData`, `m_routeData`) has unsaved changes, an ImGui modal popup offers three choices: **Save & Exit** (save all dirty data then quit), **Don't Save** (discard and quit), **Cancel** (resume editing). If nothing is dirty, the window closes immediately.

## Rendering Architecture

The app supports two view modes (Ctrl+1 / Ctrl+2): 2D top-down and 3D perspective.

### 2D Mode

Mixed rendering: most layers use ImGui's `ImDrawList` API, navmesh uses a native DX11 GPU pipeline injected via `ImDrawList::AddCallback`.

- **Background draw list** — minimap tiles (textured quads), navmesh (DX11 callback), grid lines
- **DX11 navmesh pipeline** — `NavmeshPipeline` compiles embedded HLSL at startup. `NavmeshRenderer` maintains a GPU tile cache with per-tile immutable vertex buffers (detail + LOD base polygons). Vertex shader transforms world coords to NDC; pixel shader renders barycentric wireframe. Injected into ImGui's background draw list via `AddCallback` / `ImDrawCallback_ResetRenderState`
- **Window draw list** — grid, road graph overlay, graph nodes/edges, routes, paths (layered by render order)
- **ImGui windows** — panels, menus, status bar (standard ImGui widgets)

### 3D Mode

Direct DX11 rendering to RTV+DSV before ImGui overlay. Render order (back to front):

1. **Ground plane** (`GroundPlane3D`) — minimap tile textures as flat quads at Z=0, depth write OFF
2. **Terrain** (`TerrainRenderer`) — opaque heightmap mesh, depth write ON
3. **Buildings** (`BuildingRenderer`) — opaque M2/WMO geometry, depth write ON
4. **Navmesh** (`NavmeshRenderer3D`) — semi-transparent colored polygons, depth write OFF, LESS_EQUAL depth test (overlays terrain)
5. **Overlays** (`Primitives3D`) — grid, graph nodes/edges, routes, paths, player marker (line/circle primitives batched into a single draw call)
6. **ImGui** — UI panels rendered as overlay on top of 3D scene
7. **FPS limiter** — spin-wait with `_mm_pause()` before `Present()` (when enabled)

The 3D camera provides perspective projection with orbit controls. A depth-stencil buffer (D24_UNORM_S8) is created alongside the backbuffer.

### Performance Profiler

`FrameProfiler` (header-only) wraps each render layer with `QueryPerformanceCounter` timing, averaged over 60 frames. Each renderer exposes `statDrawCalls` / `statVertices` counters updated during `Render()`. The "Performance" ImGui overlay shows per-layer CPU ms, draw calls, vertex counts, VSync toggle, and FPS limiter slider (0-300, spin-wait based).

Layer visibility is controlled by `LayerVisibility` struct, toggled in the Layer Panel.

## Threading Model

- **Main thread** — Win32 message loop, ImGui frame, all rendering, MPQ reads, D3D11 calls
- **Minimap worker thread** — BLP texture decoding (CPU-bound DXT decompression)
- **Terrain worker threads (×3)** — `.map` file parsing, mesh generation, ADT texture compositing, BC1 compression. Each worker has its own `AdtTextureParser`/`BlpTextureCache`/`TerrainTextureCompositor` instances (worker-local, no shared state). MPQ reads serialized via `m_mpqMutex` (StormLib not thread-safe for concurrent reads). BC1 compression runs outside all locks for true parallelism.
- **Building worker thread** — `.vmtile` parsing + M2/WMO loading + geometry assembly (disk I/O + CPU)

All worker threads communicate via mutex-protected request/result queues with condition variable signaling. D3D11 resource creation (textures, vertex/index buffers) happens on main thread only, throttled to a max number of uploads per frame to avoid stalls.
