# Using Road Extraction JSON in Map Editor

## Overview

The Python road extraction pipeline (`docs/scripts/road_extraction/`) produces a `{Map}_roads.json` file in the map-editor's native `WorldGraphData` format. No conversion needed — the output is directly loadable.

## Quick Start

1. Extract roads:
   ```bash
   cd map-editor/docs/scripts/road_extraction
   python -m src.main --client "Z:\Games\wow 3.3.5a client" --map Azeroth --all
   ```

2. Open map-editor, menu **Graph > Open Road Graph...**, select `output/Azeroth_roads.json`.

3. Road network renders immediately as a semi-transparent amber overlay on the 2D canvas (underneath the main world graph).

4. Toggle visibility in the **Layers** panel via the "Road Graph" checkbox.

> **Note**: The road graph is loaded into a dedicated slot, separate from the main world graph. You can have both open simultaneously — the road network provides spatial context while the POI graph sits on top. Switch to **Road Graph** editing mode via the toolbar button to edit it directly.

## Output Format

The extraction script outputs JSON identical to what the editor reads and writes:

```json
{
  "nodes": [
    { "id": 1, "name": "", "map": 0, "x": -9465.12, "y": 62.34, "z": 0.0, "type": "waypoint" }
  ],
  "edges": [
    { "from": 1, "to": 2, "type": "walk", "cost": 6.42, "bidir": true }
  ],
  "metadata": { ... }
}
```

All fields match the C++ `WorldGraphData` loader (`world_graph_data.cpp`):

| Field | Value | Notes |
|---|---|---|
| Node `id` | Sequential from 1 | |
| Node `name` | `""` | Empty — can be filled in editor |
| Node `map` | mapId (0, 1, 530, 571) | Auto-set from `--map` argument |
| Node `x`, `y` | WoW world coords | Road centerline positions |
| Node `z` | `0.0` | No height from alpha maps; use **Tools > Assign Terrain Heights** to populate |
| Node `type` | `"waypoint"` | |
| Edge `type` | `"walk"` | |
| Edge `cost` | Euclidean distance (yards) | Between endpoints |
| Edge `bidir` | `true` | Roads are bidirectional |

The `metadata` section is kept for reference but ignored by the C++ loader.

### Map IDs

The `--map` name is automatically mapped to the correct WoW DBC mapId:

| `--map` | mapId | Continent |
|---|---|---|
| `Azeroth` | 0 | Eastern Kingdoms |
| `Kalimdor` | 1 | Kalimdor |
| `Expansion01` | 530 | Outland |
| `Northrend` | 571 | Northrend |

## Assigning Terrain Heights

Extracted road nodes have `z=0` because alpha maps contain no elevation data. To assign real terrain heights:

1. Set the mmaps directory (**File > Set mmaps Directory**) — this auto-detects the TC data path containing `.map` heightmap files
2. Load the road graph (**Graph > Open Road Graph...**)
3. **Tools > Assign Terrain Heights** — batch-updates all nodes on the current map with terrain-sampled Z values
4. Save (**Graph > Save Road Graph** or `Ctrl+S`)

After assigning heights, road graph nodes sit on the terrain surface and render correctly in 3D mode.

New nodes created via the editor (double-click, draw mode, split edge, context menu) automatically receive terrain-sampled heights when the TC data path is set.

## Known Limitations

- **Straight edges** — the editor renders edges as straight lines between nodes. Node density compensates — junctions are close enough that straight lines approximate curves

## Editing Road Graphs

The road graph is directly editable. Click the toolbar mode button to switch to **Road Graph** mode (orange). All standard editor tools apply:

| Action | How |
|---|---|
| Switch to Road Graph mode | Click toolbar button until "Road Graph" (orange) |
| Select node | Click on it |
| Multi-select | Shift+Click, box drag, Alt+drag (lasso), Ctrl+A |
| Move nodes | Drag (moves all selected) |
| Delete | Select + `Delete` |
| Add node | Double-click on empty canvas |
| Draw mode | `D` — click to place chain of connected nodes |
| Edge mode | `E` — click two nodes to connect |
| Split edge | `S` — split nearest edge at cursor |
| Context menu | Right-click — Delete, Connect, Split, Merge, Straighten, Validate |
| Auto-connect | Right-click > Auto-Connect Nearby (joins close endpoints) |
| Validate | Right-click > Validate Graph (find disconnected components, dead-ends, orphans) |
| Undo / Redo | `Ctrl+Z` / `Ctrl+Shift+Z` (independent from world graph undo) |
| Save | **Graph > Save Road Graph** or **Save Road Graph As...** |

## Recommended Workflow

1. **Extract** — run the pipeline for the target continent:
   ```bash
   python -m src.main --client "..." --map Azeroth --all
   python -m src.main --client "..." --map Northrend --all
   ```

2. **Load as overlay** — open `Azeroth_roads.json` via **Graph > Open Road Graph...** (read-only overlay, displayed beneath the main world graph)

3. **Review** — pan/zoom to areas of interest, check road connectivity visually.

4. **Clean up** — delete false-positive nodes (water, buildings, dungeon interiors), fix broken connections at tile boundaries.

5. **Add special nodes** — place `flight_master`, `innkeeper`, `zone_boundary`, `dungeon` nodes at key locations. Change the type in the property panel.

6. **Connect systems** — add `flight` edges between flight masters, `teleport` edges at portals, `boat` edges for boat/zeppelin routes.

7. **Save** — `Ctrl+S`. The editor overwrites the JSON with all fields populated. No further conversion needed.

## File Locations

| What | Path |
|---|---|
| Extraction script | `map-editor/docs/scripts/road_extraction/` |
| Extraction output | `map-editor/docs/scripts/road_extraction/output/{Map}_roads.json` |
| C++ graph data model | `map-editor/src/data/world_graph_data.h` / `.cpp` |
| C++ graph renderer (2D) | `map-editor/src/render/graph_renderer.cpp` |
| C++ graph renderer (3D) | `map-editor/src/render/graph_renderer_3d.cpp` |
| C++ graph editor | `map-editor/src/editor/graph_editor.cpp` |
| C++ terrain height sampler | `map-editor/src/data/terrain_height_sampler.h` / `.cpp` |
