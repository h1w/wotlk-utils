# Road Graph Overlay (2D)

**Date:** 2026-02-25
**Task:** TASK-006

## Summary

Added a dedicated road graph slot that loads `_roads.json` files as a read-only 2D overlay, displayed beneath the main world graph. Both graphs are now visible simultaneously — the road network provides spatial context while the hand-crafted POI/navigation graph sits on top.

## Problem

TASK-005 produced road network JSON files in `WorldGraphData` format, but the editor only supported a single graph at a time. Users had to choose between their hand-crafted POI world graph and the auto-generated road network — they could not view both simultaneously.

## Solution

Added a second `WorldGraphData` instance (`m_roadGraphData`) with a dedicated menu item, persistent settings, layer toggle, and a simplified rendering path.

### Menu

- **Graph > Open Road Graph...** — opens a file dialog (JSON filter) to load a road network file
- Placed above the existing Save/Save As items with a separator

### Rendering

- Road graph renders as a 2D overlay BEFORE the main world graph (roads underneath, POI on top)
- Dedicated `GraphRenderer::RenderRoadOverlay()` — simplified version of `Render()`:
  - **Edges**: `IM_COL32(200, 140, 50, 120)` (semi-transparent amber), 1.0px thickness
  - **Nodes**: 3px radius, `IM_COL32(180, 130, 60, 160)` (muted brown), no outline
  - **No labels** — road waypoints have empty names
  - **No selection** — road graph is read-only, not connected to editors or undo/redo
- Frustum culled via `canvas.GetViewBounds()`
- **2D only** — not rendered in 3D mode (road nodes have z=0, no useful height data)

### Layer Panel

- "Road Graph" checkbox added between "Labels" and "Routes"
- Defaults to visible (`showRoadGraph = true`)

### Settings Persistence

- `road_graph_path` saved/loaded in `app_settings.json`
- `road_graph` layer visibility saved under `layers` section
- Road graph auto-loads on startup if path is set

## Modified Files

| File | Changes |
|------|---------|
| `src/data/app_settings.h` | Added `roadGraphPath`, `showRoadGraph` |
| `src/data/app_settings.cpp` | Serialize/deserialize both new fields |
| `src/ui/main_menu.h` | Added `openRoadGraph`, `roadGraphFilePath` to Actions |
| `src/ui/main_menu.cpp` | Added "Open Road Graph..." menu item |
| `src/render/graph_renderer.h` | Added `showRoadGraph` to `LayerVisibility`, declared `RenderRoadOverlay()` |
| `src/render/graph_renderer.cpp` | Implemented `RenderRoadOverlay()` |
| `src/ui/layer_panel.cpp` | Added "Road Graph" checkbox |
| `src/app.h` | Added `WorldGraphData m_roadGraphData` |
| `src/app.cpp` | Load/save road graph, render in 2D, handle menu action |

**No new files created.**

## Usage

1. Extract roads via the Python pipeline (TASK-005):
   ```bash
   cd map-editor/docs/scripts/road_extraction
   python -m src.main --client "Z:\Games\wow 3.3.5a client" --map Azeroth --all
   ```

2. In map-editor: **Graph > Open Road Graph...** → select `output/Azeroth_roads.json`

3. Load your POI graph via **Graph > Open Graph JSON...** as usual

4. Both graphs visible simultaneously. Toggle road graph in Layer panel.
