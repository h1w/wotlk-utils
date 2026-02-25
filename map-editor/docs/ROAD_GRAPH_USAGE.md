# Using Road Extraction JSON in Map Editor

## Overview

The Python road extraction pipeline (`docs/scripts/road_extraction/`) produces a `{Map}_roads.json` file in the map-editor's native `WorldGraphData` format. No conversion needed — the output is directly loadable.

## Quick Start

1. Extract roads:
   ```bash
   cd map-editor/docs/scripts/road_extraction
   python -m src.main --client "Z:\Games\wow 3.3.5a client" --map Azeroth --all
   ```

2. Open map-editor, menu **Graph > Open Graph JSON...**, select `output/Azeroth_roads.json`.

3. Nodes and edges render immediately on the 2D canvas and in 3D view.

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
| Node `z` | `0.0` | No height from alpha maps |
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

## Known Limitations

- **Z = 0** — nodes have no height data (alpha maps don't contain elevation). In 3D view, road nodes float at ground zero; they may clip through or hover above terrain.
- **Straight edges** — the editor renders edges as straight lines between nodes. The extraction pipeline's intermediate waypoints (from skeletonization) are not stored in the output because `WorldGraphData` has no `waypoints` field on edges. Node density compensates — junctions are close enough that straight lines approximate curves.

## Editing Imported Roads

The imported graph is fully editable with all standard editor tools:

| Action | How |
|---|---|
| Select node | Click on it |
| Move node | Drag |
| Delete node | Select + `Delete` (removes connected edges too) |
| Add node | Double-click on empty canvas |
| Create edge | `E` (edge mode), click first node, click second node |
| Delete edge | Select edge + `Delete` |
| Edit properties | Select node/edge, modify in property panel |
| Undo / Redo | `Ctrl+Z` / `Ctrl+Y` |
| Save | `Ctrl+S` or **Graph > Save** |

## Recommended Workflow

1. **Extract** — run the pipeline for the target continent:
   ```bash
   python -m src.main --client "..." --map Azeroth --all
   python -m src.main --client "..." --map Northrend --all
   ```

2. **Load** — open `Azeroth_roads.json` in map-editor via **Graph > Open Graph JSON...**

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
