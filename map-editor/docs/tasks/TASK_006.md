# TASK-006: Road Graph Overlay in Map Editor (2D)

**Status**: DONE
**Created**: 2026-02-25

---

## Overview

Add support for loading a second graph file (road network from `_roads.json`) alongside the existing world graph. The road graph renders as a read-only 2D overlay, visually distinct from the main world graph. No 3D rendering, no editing.

**Context**: TASK-005 produced a Python pipeline that extracts road networks from ADT terrain files into `{Map}_roads.json` — same `WorldGraphData` JSON format. Currently the editor can only load one graph at a time via **Graph > Open Graph JSON...**. The user must choose between their hand-crafted POI/navigation world graph and the auto-generated road network. This task adds a dedicated road graph slot so both can be displayed simultaneously.

**Goal**: Display road network overlay on the 2D canvas alongside the existing world graph, loaded from a separate JSON file.

---

## Requirements

### 1. Data — Second `WorldGraphData` instance

- Add `WorldGraphData m_roadGraphData` to `App` (alongside existing `m_graphData`)
- Road graph is **read-only** in the editor — no selection, no drag, no undo/redo integration
- Reuse the existing `WorldGraphData::LoadFromFile()` — no new parser needed

### 2. Menu — "Open Road Graph..."

- Add menu item **Graph > Open Road Graph...** in `main_menu.cpp`
- Uses the same `OpenFileDialog` with JSON filter
- Add corresponding action fields to `MainMenu::Actions`:
  - `bool openRoadGraph = false`
  - `std::string roadGraphFilePath`

### 3. Settings — Persist road graph path

- Add `std::string roadGraphPath` to `AppSettings`
- Save/load in `app_settings.cpp` (same pattern as `worldGraphPath`)
- On startup, auto-load road graph if path is set

### 4. 2D Rendering — Visual distinction

- Render road graph BEFORE the main world graph (roads underneath, POI on top)
- Call `GraphRenderer::Render()` (or a dedicated thin wrapper) with `m_roadGraphData`
- Visual style differences from main graph:
  - **Edges**: warm color (e.g., `IM_COL32(200, 140, 50, 120)` — semi-transparent orange/brown), thinner (1.0px)
  - **Nodes**: smaller radius (3px vs 6px), muted color (e.g., `IM_COL32(180, 130, 60, 160)`)
  - **No labels** — road waypoints have empty names anyway
  - **No selection highlight** — road graph is not selectable
- Skip rendering if `m_roadGraphData` is not loaded

### 5. Layer Panel — Toggle visibility

- Add `bool showRoadGraph = true` to `LayerVisibility`
- Add "Road Graph" checkbox in Layer Panel UI
- Road graph rendering respects this toggle

### 6. No 3D rendering

- Road graph is **2D only** — do not call `Graph3DRenderer` for road data
- Road nodes have z=0, no useful height data

---

## Implementation Plan

### Phase 1: Data + Menu + Settings
1. Add `openRoadGraph` / `roadGraphFilePath` to `MainMenu::Actions`
2. Add "Open Road Graph..." menu item in `main_menu.cpp`
3. Add `roadGraphPath` to `AppSettings`, serialize/deserialize in `app_settings.cpp`
4. Add `WorldGraphData m_roadGraphData` to `App`
5. Wire up loading in `App::RenderFrame()` (menu action) and `App::LoadSettings()` (startup)
6. Save road graph path in `App::SaveSettings()`

### Phase 2: 2D Rendering + Layer Toggle
1. Add `showRoadGraph` to `LayerVisibility`
2. Add checkbox in `LayerPanel::Render()`
3. In `App::RenderFrame2D()`, render road graph before main graph:
   ```cpp
   if (m_roadGraphData.IsLoaded() && m_layers.showRoadGraph)
       m_graphRenderer.RenderRoadOverlay(m_canvas, m_roadGraphData, m_currentMapId);
   ```
4. Implement `GraphRenderer::RenderRoadOverlay()` — simplified version of `Render()`:
   - No selection parameter
   - Hardcoded road-specific colors and sizes
   - No label rendering

---

## Files to Modify

| File | Changes |
|---|---|
| `src/app.h` | Add `WorldGraphData m_roadGraphData` |
| `src/app.cpp` | Load/save road graph, render in 2D loop |
| `src/ui/main_menu.h` | Add `openRoadGraph` + `roadGraphFilePath` to Actions |
| `src/ui/main_menu.cpp` | Add "Open Road Graph..." menu item |
| `src/data/app_settings.h` | Add `roadGraphPath` field |
| `src/data/app_settings.cpp` | Serialize/deserialize `roadGraphPath` |
| `src/render/graph_renderer.h` | Add `RenderRoadOverlay()` declaration |
| `src/render/graph_renderer.cpp` | Implement `RenderRoadOverlay()` |
| `src/ui/layer_panel.h` | Add `showRoadGraph` to `LayerVisibility` |
| `src/ui/layer_panel.cpp` | Add "Road Graph" checkbox |

**No new files needed.**

---

## Out of Scope

- Road graph editing (selection, drag, delete, undo/redo)
- 3D rendering of road graph
- Merging road graph into world graph
- Road graph-specific property panel
