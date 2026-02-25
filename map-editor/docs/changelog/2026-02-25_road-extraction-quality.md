# Road Extraction Quality — False Positive Filtering, Water Masking & Curve Preservation

**Date:** 2026-02-25
**Task:** TASK-007

## Summary

Major quality overhaul of the road extraction pipeline (TASK-005). Fixed critical bugs that made the pipeline unusable, added water zone filtering, hardened the texture classifier, improved morphological cleanup, added graph post-processing, and preserved road curvature in the output graph. The pipeline now produces clean, accurate road networks that closely follow actual road geometry.

Before: ~60-70% of waypoints were garbage (water, forest floor, noise). Roads detected in mountains but not in towns.
After: 187/687 Azeroth tiles with roads, 6016 nodes, 5905 edges, 0 errors. Waypoints follow road curves precisely.

## Critical Bug Fixes

### MH2O Water Offset Bug (Root Cause of Missing Roads)

**File**: `src/adt_parser.py`

The `_parse_mh2o_for_chunks()` function read the MH2O pointer from MHDR offset `0x14`, which is actually the **MWMO** field (WMO model paths), not MH2O (water data). This caused the water parser to interpret WMO path strings as liquid chunk data, marking 66-72% of each tile as water. The water mask then subtracted most road pixels, leaving only mountain tiles (which have few WMOs) showing any roads.

```
MHDR layout:
  0x00=flags, 0x04=mcin, 0x08=mtex, 0x0C=mmdx, 0x10=mmid,
  0x14=mwmo  ← BUG: code read here
  0x18=mwid, 0x1C=mddf, 0x20=modf, 0x24=mfbo,
  0x28=mh2o  ← CORRECT offset
```

**Fix**: Changed `mhdr_off + 0x14` to `mhdr_off + 0x28`.

**Impact**: Water coverage dropped from 72% to 7% per tile. Road detection went from 28 tiles to 187 tiles.

### MH2O SLiquidChunk Size

The SLiquidChunk struct was parsed as 24 bytes (sizeof SLiquidInstance). Correct size is **12 bytes** (3 x uint32: offset_instances, layer_count, offset_attributes).

### collapse_parallel_paths Endpoint Reconnection

When removing the shorter of two parallel chains, the function deleted interior nodes but did not reconnect the chain's endpoints to the surviving chain, leaving dangling nodes.

**Fix**: Added `edges_to_add` list that connects dropped chain endpoints to the corresponding endpoints of the kept chain.

## Phase 1: Texture Classifier Hardening

**File**: `src/road_classifier.py`

- Removed `"dirt"` from `ROAD_PATTERNS_MEDIUM` — standalone dirt textures (`Duskwood_Dirt01.blp`) are natural terrain, not roads. `"dirtroad"` still matches via HIGH patterns
- Added `"trail"` to medium patterns
- Expanded `EXCLUDE_PATTERNS` with 20+ new entries: water/coast terms (`beach`, `shore`, `river`, `lake`, `ocean`), natural terrain (`ground`, `floor`, `terrain`, `earth`, `farmland`), structures (`cave`, `cavern`, `mine`, `rubble`, `debris`, `ruin`)

**File**: `src/road_mask.py`

- Added `min_confidence` parameter to `build_road_mask()` — only textures above the threshold are included
- Default `min_confidence=0.5` (was effectively 0.0)

**File**: `src/main.py`

- Added `--min-confidence` CLI parameter

## Phase 2: Water Zone Filtering (MH2O)

**File**: `src/adt_parser.py`

- Added `_parse_mh2o_for_chunks()` — parses MH2O liquid headers via MHDR offset
- Parses SLiquidChunk (12 bytes) and SLiquidInstance (24 bytes) including exists-bitmap
- Builds 8x8 boolean liquid bitmap per MCNK chunk
- Added `has_liquid`, `liquid_coverage`, `liquid_bitmap` fields to `McnkData`

**File**: `src/road_mask.py`

- Added `build_water_mask()` — expands 8x8 liquid bitmaps to 64x64 pixels per chunk, assembles 1024x1024 water mask, applies configurable dilation buffer
- Water mask subtracted from road mask before graph extraction

**File**: `src/main.py`

- Added `--water-buffer` CLI parameter (default 5 pixels = ~2.5 yards)
- Diagnostic output: water pixel count, road pixel survival rate

## Phase 3: Morphological Improvements

**File**: `src/graph_extractor.py`

- `cleanup_mask()`: close radius 1 -> 3 (bridges 3px chunk boundary gaps), open radius 1 -> 1, min object size 20 -> 50 pixels
- Added `filter_by_width()` — distance transform + dilation to remove regions thinner than N pixels
- Spur pruning: 3 -> 8 iterations (removes skeleton branches up to ~4 yards)

**File**: `src/main.py`

- Added CLI parameters: `--close-radius`, `--open-radius`, `--min-road-size`, `--min-road-width`, `--prune-iterations`
- Added `--diagnose` mode for per-tile pipeline diagnostics

## Phase 4: Graph Post-Processing

**File**: `src/graph_extractor.py`

### New Functions

- `merge_close_nodes()` — spatial-hash union-find to collapse nodes within distance threshold into single junction nodes. Default 4.0 yards (reduced from 8.0 to preserve curve detail at intersections)
- `remove_short_components()` — removes connected components with total edge length < 30 yards
- `collapse_parallel_paths()` — detects and merges parallel degree-2 chains within 10 yards separation
- `expand_edge_polylines()` — **KEY FIX**: converts sknw edge polyline points (`world_pts`) into actual graph nodes. Previously, sknw stored intermediate skeleton points as edge attributes, but the JSON export only wrote node endpoints — all curve detail was lost. Now each edge's polyline points become real waypoint nodes, creating chains that follow road curves precisely
- `eliminate_degree2_nodes()` — curvature-preserving version: only removes degree-2 nodes where perpendicular deviation from line A-C is < 2 yards AND resulting edge < 30 yards. Nodes at road bends are preserved
- `_point_line_distance()` — helper for perpendicular distance calculation

### Updated Pipeline

```
1. cleanup_mask (close=3, open=1, min_size=50)
2. filter_by_width (optional)
3. skeletonize + prune (8 iterations)
4. skeleton_to_graph (sknw)
5. assign_world_coords
6. simplify_edges (Douglas-Peucker, epsilon=2.0 yards)
7. merge_close_nodes (4.0 yards)
8. remove_short_components (30 yards min)
9. collapse_parallel_paths (10 yards)
10. expand_edge_polylines  ← NEW: polyline pts → real nodes
11. eliminate_degree2_nodes (conservative: deviation<2, edge<30)
```

## Phase 5: Tile Boundary Merge Fix

**File**: `src/main.py`

- Increased `merge_tile_graphs_spatial()` merge distance from 2.0 to 5.0 yards — fixes road gaps at tile boundaries where skeleton endpoints were 3-5 yards apart

## Results

| Metric | Before (TASK-005) | After (TASK-007) |
|--------|-------------------|-------------------|
| Tiles with roads | ~28 (mostly mountains) | 187 (correct) |
| Nodes | ~1,700 (garbage) | 6,016 (accurate) |
| Edges | ~1,400 | 5,905 |
| False positive rate | ~60-70% | <5% estimated |
| Road curve fidelity | Straight lines between junctions | Follows road bends |
| Water false positives | Hundreds of ocean/river waypoints | Zero |
| Tile boundary gaps | Frequent | Rare |
| Errors/warnings | Multiple | 0 |

## Parameter Tuning Guide

| Problem | Solution |
|---------|----------|
| False roads in forests | Raise `--min-confidence` to 0.8 or 1.0 |
| Real roads missing | Lower `--threshold` from 64 to 48, or add zone includes |
| Gaps in continuous roads | Increase `--close-radius` to 5 |
| Too many noise clusters | Raise `--min-road-size` to 200 |
| Curves too angular | Lower `--dp-epsilon` to 1.0 |
| Tile boundary gaps | Already fixed (merge_distance=5.0) |
