# Road Network Extraction Pipeline

**Date:** 2026-02-25
**Task:** TASK-005, Phases 1-5

## Summary

Built a complete Python pipeline that extracts road networks from WoW 3.3.5a ADT terrain files and outputs a JSON graph directly loadable by the map-editor. The pipeline reads MPQ archives, parses ADT terrain data, classifies road textures via pattern matching, builds binary road masks from alpha maps, skeletonizes them into centerlines, and exports a world graph with WoW coordinates. Supports single tiles, multi-tile regions, and full-map extraction (all ~687 Azeroth tiles in one run).

The output JSON is in the map-editor's native `WorldGraphData` format — open via **Graph > Open Graph JSON...** with zero conversion.

## Problem

The map-editor's world graph system had no way to import road data. Roads in WoW are not discrete objects — they are alpha-blended texture layers on terrain chunks, invisible to navmesh extractors and TrinityCore tools. Building a road network manually in the editor would require placing thousands of nodes by hand.

## Solution

Created a `uv`-managed Python project at `map-editor/docs/scripts/road_extraction/` with a 10-step automated pipeline:

```
MPQ archives → WDT (alpha format) → ADT parsing → texture classification
  → alpha map decode → binary road mask → morphological cleanup
  → Zhang-Suen skeletonization → sknw graph → world coordinates
  → Douglas-Peucker simplification → tile boundary merge → JSON export
```

### Phase 1: Project Setup + WDT/ADT Parser

Created the project scaffold with `pyproject.toml`, StormLib ctypes wrapper (`mpq_reader.py`), WDT parser for alpha format flags (`wdt_parser.py`), and full ADT parser (`adt_parser.py`) covering MCNK headers, MTEX texture lists, MCLY layer definitions, and MCAL alpha map decode. The `mcal_decoder.py` supports all three alpha formats: 4-bit packed (Eastern Kingdoms), 8-bit uncompressed (Northrend), and RLE compressed.

### Phase 2: Road Texture Classification

Built `road_classifier.py` with two-tier pattern matching: high-confidence patterns (`road`, `cobble`, `flagstone`, `paving`) and medium-confidence patterns (`dirt`, `path`, `gravel`) with exclusion filters (`grass`, `forest`, `rock`, etc.). Supports per-zone overrides via `config/road_textures.json`. The `road_mask.py` module combines classification with alpha thresholding to produce a 1024x1024 binary road mask per tile.

### Phase 3: Road Visualization + Multi-Tile Stitching

Created `visualizer.py` for per-tile PNG output (binary mask + confidence color-coded overlay) and `stitcher.py` for multi-tile image assembly with automatic downscaling for large regions.

### Phase 4: Road Graph Extraction

Built `graph_extractor.py` — the core of the pipeline:
- Morphological cleanup (close gaps, remove noise, drop tiny regions)
- Zhang-Suen skeletonization via scikit-image (1px-wide centerlines)
- Spur pruning (remove short false branches at endpoints)
- sknw graph construction (skeleton pixels → NetworkX graph with junction nodes and edge polylines)
- World coordinate assignment (pixel → WoW X/Y via tile origin + pixel size)
- Douglas-Peucker edge simplification (default 2-yard tolerance)
- Multi-tile merge at boundaries (nodes within 2 yards from different tiles get merged)

### Phase 5: Full-Map Extraction + Map-Editor Integration

Added `--all` CLI flag for full-map extraction. Processes all tiles from the WDT grid in a streaming fashion (one tile at a time, mask/ADT discarded after graph extraction). Added `merge_tile_graphs_spatial()` with spatial hashing — O(N) amortized instead of O(N^2) all-pairs comparison — to handle thousands of nodes across hundreds of tiles.

Updated `export_graph_json()` to output the map-editor's native `WorldGraphData` format directly:
- Nodes: `id` (from 1), `name`, `map` (correct WoW mapId), `x`, `y`, `z`, `type`
- Edges: `from`, `to`, `type` ("walk"), `cost` (Euclidean distance in yards), `bidir` (true)
- Map ID auto-detection: Azeroth=0, Kalimdor=1, Expansion01=530, Northrend=571

## New Files

| File | Description |
|------|-------------|
| `docs/scripts/road_extraction/pyproject.toml` | uv project config with all dependencies |
| `docs/scripts/road_extraction/README.md` | Setup, usage, CLI reference, output format docs |
| `docs/scripts/road_extraction/config/road_textures.json` | Per-zone road texture classification rules |
| `docs/scripts/road_extraction/src/__init__.py` | Package init |
| `docs/scripts/road_extraction/src/main.py` | CLI entry point (`--tiles`, `--all`, `--dump-info`, etc.) |
| `docs/scripts/road_extraction/src/mpq_reader.py` | StormLib ctypes wrapper for MPQ archive access |
| `docs/scripts/road_extraction/src/wdt_parser.py` | WDT parser (MPHD flags, tile existence grid) |
| `docs/scripts/road_extraction/src/adt_parser.py` | ADT parser (MCNK/MTEX/MCLY/MCAL) |
| `docs/scripts/road_extraction/src/mcal_decoder.py` | Alpha map decoder (4-bit, 8-bit, RLE) |
| `docs/scripts/road_extraction/src/coord_utils.py` | WoW coordinate constants and pixel-to-world mapping |
| `docs/scripts/road_extraction/src/road_classifier.py` | Texture name → road/non-road classification |
| `docs/scripts/road_extraction/src/road_mask.py` | Alpha layers → binary road mask (1024x1024) |
| `docs/scripts/road_extraction/src/graph_extractor.py` | Mask → skeleton → graph → merge → JSON export |
| `docs/scripts/road_extraction/src/stitcher.py` | Multi-tile image stitching |
| `docs/scripts/road_extraction/src/visualizer.py` | Per-tile PNG output (mask + confidence overlay) |
| `docs/ROAD_GRAPH_USAGE.md` | Guide for using road JSON in the map-editor |

## Technical Details

### Alpha Map Formats

The pipeline handles all three WoW 3.3.5a alpha map formats transparently:

| Format | Size | Used by | Decode |
|--------|------|---------|--------|
| 4-bit packed | 2048 bytes | Eastern Kingdoms, Kalimdor | Low nibble first, expand 0-15 → 0-255 |
| 8-bit raw | 4096 bytes | Northrend | Direct `np.frombuffer` reshape |
| RLE compressed | Variable → 4096 | Per-layer flag `0x200` | Fill/copy modes, cap at 4096 output |

Format selection is automatic: WDT MPHD flag `0x0004` → 8-bit; MCLY flag `0x200` → RLE; else → 4-bit.

### Skeletonization → Centerline Accuracy

Zhang-Suen skeletonization produces a 1-pixel-wide centerline from the binary road mask. sknw's `graph[s][e]['pts']` are these centerline pixels. After pixel-to-world conversion, waypoints sit at road center by construction — no additional centering logic needed.

### Spatial Hash Merge

For full-map extraction with thousands of nodes, `merge_tile_graphs_spatial()` replaces the O(N^2) brute-force:

1. Combine all tile graphs with globally unique node IDs, tag each node with source tile index
2. Build spatial grid: cell_size = merge_distance, each node hashed to `(floor(x/cell), floor(y/cell))`
3. For each node, check same cell + 8 neighbors; only merge nodes from different source tiles within distance threshold
4. Union-find to build merge groups, average positions, remap edges

### Output Compatibility

The JSON output matches the C++ `WorldGraphData::LoadFromFile()` field-by-field:

```cpp
node.id      = jn.value("id", 0u);       // ← set by pipeline
node.name    = jn.value("name", "");      // ← "" (empty)
node.mapId   = jn.value("map", 0u);       // ← correct mapId
node.x       = jn.value("x", 0.0f);      // ← world coords
node.y       = jn.value("y", 0.0f);      // ← world coords
node.z       = jn.value("z", 0.0f);      // ← 0.0 (no height)
node.type    = ParseNodeType("waypoint"); // ← waypoint
edge.type    = ParseEdgeType("walk");     // ← walk
edge.cost    = je.value("cost", 0.0f);   // ← Euclidean distance
edge.bidir   = je.value("bidir", true);  // ← true
```

## Usage

```bash
cd map-editor/docs/scripts/road_extraction

# Setup
uv sync

# Single tile (Goldshire)
python -m src.main --client "Z:\Games\wow 3.3.5a client" --map Azeroth --tiles 31,49

# Full Eastern Kingdoms
python -m src.main --client "Z:\Games\wow 3.3.5a client" --map Azeroth --all

# Northrend
python -m src.main --client "Z:\Games\wow 3.3.5a client" --map Northrend --all
```

Then open `output/Azeroth_roads.json` in map-editor via **Graph > Open Graph JSON...**

## Known Limitations

- **Z = 0**: alpha maps contain no elevation data; all nodes are at ground zero
- **Straight edges**: the editor's `WorldEdge` has no waypoint polyline field; edges render as straight lines between junction nodes
- **No DBC chain**: texture classification uses filename patterns only (GroundEffectTexture.dbc lookup deferred — pattern matching alone achieves high accuracy)
