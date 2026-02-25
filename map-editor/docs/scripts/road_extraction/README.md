# Road Extraction Pipeline

Extract road networks from WoW 3.3.5a ADT terrain files into a JSON graph directly loadable by the map-editor.

## How It Works

1. Opens MPQ archives from a WoW 3.3.5a client
2. Parses WDT (tile index) and ADT files (terrain + alpha maps)
3. Classifies textures as road/non-road using pattern matching + optional config
4. Builds a binary road mask (1024x1024 per tile) from alpha map layers
5. Skeletonizes the mask (Zhang-Suen) to get 1px-wide centerlines
6. Converts skeleton to a NetworkX graph (nodes at junctions, edges along road segments)
7. Assigns WoW world coordinates to all nodes and edge waypoints
8. Simplifies edges with Douglas-Peucker
9. Merges tile boundaries (spatial hash for full-map, brute-force for small tile sets)
10. Exports JSON in map-editor `WorldGraphData` format (ready to open via **Graph > Open Graph JSON...**)

Waypoints are placed exactly at road center by construction (skeleton = centerline of the binary mask).

## Requirements

- Python 3.10+
- [uv](https://docs.astral.sh/uv/) (recommended) or pip
- `StormLib.dll` in system PATH or working directory (for MPQ reading via ctypes)
- WoW 3.3.5a client with MPQ data files

## Setup

```bash
cd map-editor/docs/scripts/road_extraction

# Using uv (recommended)
uv sync

# Or using pip
python -m venv .venv
.venv\Scripts\activate      # Windows
pip install -e .
```

## Usage

All commands are run from the `road_extraction/` directory.

### Single Tile

Extract roads from one tile (e.g., Goldshire area):

```bash
python -m src.main --client "Z:\Games\wow 3.3.5a client" --map Azeroth --tiles 31,49
```

Output: `output/Azeroth_roads.json` + per-tile PNG visualizations in `output/`.

### Multiple Tiles

```bash
python -m src.main --client "Z:\Games\wow 3.3.5a client" --map Azeroth --tiles 31,48 31,49 32,48 32,49
```

Generates individual tile images, a stitched overview, and a merged road graph JSON.

### Full Map (`--all`)

Process every tile in the map at once:

```bash
python -m src.main --client "Z:\Games\wow 3.3.5a client" --map Azeroth --all
```

This streams tiles one-by-one (memory-efficient), skips image generation, and uses spatial-hash merging to handle thousands of nodes. Progress is printed per tile:

```
Processing 687 tiles for map Azeroth...
  [1/687] (27,25) 4 nodes, 3 edges
  [2/687] (28,25) no roads
  ...
  [687/687] (56,54) 8 nodes, 7 edges

Merging 203 tile graphs (spatial merge)...
  Merged: 4521 nodes, 5103 edges

Exported: output/Azeroth_roads.json
  4521 nodes, 5103 edges
  Tiles: 203/687 with roads (0 skipped)
Done!
```

Works for any map: `Azeroth`, `Northrend`, `Expansion01` (Outland), etc.

### Debug / Inspect

Dump texture info without extracting anything:

```bash
python -m src.main --client "Z:\Games\wow 3.3.5a client" --map Azeroth --tiles 31,49 --dump-info
```

Generate masks and images but skip graph extraction:

```bash
python -m src.main --client "Z:\Games\wow 3.3.5a client" --map Azeroth --tiles 31,49 --no-graph
```

## CLI Reference

| Argument | Default | Description |
|---|---|---|
| `--client` | *(required)* | Path to WoW 3.3.5a client directory |
| `--map` | `Azeroth` | Map internal name (also sets mapId in output) |
| `--tiles X,Y [...]` | | Specific tile coordinates (mutually exclusive with `--all`) |
| `--all` | | Process all tiles in the map (mutually exclusive with `--tiles`) |
| `--threshold` | `64` | Alpha threshold for road detection (0-255) |
| `--output-dir` | `./output` | Output directory |
| `--config` | `config/road_textures.json` | Road texture classification config |
| `--dump-info` | off | Only dump tile/texture info, no extraction |
| `--no-graph` | off | Generate masks/images but skip graph extraction |
| `--prune-iterations` | `3` | Spur pruning iterations (remove short dead-end branches) |
| `--dp-epsilon` | `2.0` | Douglas-Peucker simplification tolerance in yards |

## Output Format

The JSON output (`{Map}_roads.json`) is in map-editor `WorldGraphData` format and can be opened directly via **Graph > Open Graph JSON...** without any conversion.

```json
{
  "nodes": [
    { "id": 1, "name": "", "map": 0, "x": -9465.12, "y": 62.34, "z": 0.0, "type": "waypoint" },
    { "id": 2, "name": "", "map": 0, "x": -9470.55, "y": 58.91, "z": 0.0, "type": "waypoint" }
  ],
  "edges": [
    { "from": 1, "to": 2, "type": "walk", "cost": 6.42, "bidir": true }
  ],
  "metadata": {
    "map": "Azeroth",
    "map_id": 0,
    "threshold": 64,
    "generated": "2026-02-25T12:00:00+00:00",
    "node_count": 2,
    "edge_count": 1
  }
}
```

Node fields:
- `id` — sequential, starting from 1
- `x`, `y` — WoW world coordinates (X+ = north, Y+ = west)
- `z` — always 0.0 (no height data from alpha maps)
- `map` — WoW mapId (0 = Eastern Kingdoms, 571 = Northrend, etc.)
- `type` — always `"waypoint"`

Edge fields:
- `from`, `to` — node IDs
- `type` — always `"walk"`
- `cost` — Euclidean distance between endpoints in yards
- `bidir` — always `true` (roads are bidirectional)

The `metadata` section is ignored by the editor loader and kept for reference only.

### Map IDs

| `--map` value | mapId | Continent |
|---|---|---|
| `Azeroth` | 0 | Eastern Kingdoms |
| `Kalimdor` | 1 | Kalimdor |
| `Expansion01` | 530 | Outland |
| `Northrend` | 571 | Northrend |

## Project Structure

```
road_extraction/
  config/
    road_textures.json    # texture classification rules
  src/
    main.py               # CLI entry point
    mpq_reader.py         # StormLib ctypes wrapper
    wdt_parser.py         # WDT (tile index) parser
    adt_parser.py         # ADT (terrain) parser
    mcal_decoder.py       # alpha map decoder (4-bit, 8-bit, uncompressed)
    road_classifier.py    # texture name -> road/non-road classification
    road_mask.py          # alpha layers -> binary road mask
    coord_utils.py        # WoW coordinate math
    graph_extractor.py    # mask -> skeleton -> graph -> JSON
    stitcher.py           # multi-tile image stitching
    visualizer.py         # per-tile PNG output
  output/                 # generated files (gitignored)
  pyproject.toml
```
