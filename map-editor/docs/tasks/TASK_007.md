# TASK-007: Road Extraction Quality — False Positive Filtering & Graph Cleanup

**Status**: DONE
**Created**: 2026-02-25
**Completed**: 2026-02-25
**Depends on**: TASK-005 (road extraction pipeline), TASK-006 (road graph overlay)
**Changelog**: `docs/changelog/2026-02-25_road-extraction-quality.md`

---

## Overview

The road extraction pipeline (TASK-005) produces road network graphs with severe quality problems:

1. **False positive roads in water** — waypoints appear in oceans, rivers, lakes, along coastlines where no roads exist
2. **Duplicate parallel paths** — single roads generate 2-3 parallel waypoint lines instead of one centerline
3. **Gaps in continuous roads** — roads split into disconnected segments at MCNK chunk boundaries
4. **Chaotic noise in forested zones** — Duskwood and similar areas produce hundreds of false waypoints from forest floor textures
5. **Messy intersections** — T-junctions and crossroads have clusters of redundant nodes instead of clean single junction points

Currently ~60-70% of all generated waypoints are garbage. The graph is unusable for navigation without manual cleanup of nearly every road, which is impractical at continent scale.

**Goal**: Reduce false positive rate to <10%, produce clean single-line centerline paths with simple intersection topology.

**Location**: `map-editor/docs/scripts/road_extraction/src/` (Python pipeline)

---

## Root Cause Analysis

### Problem 1: False Positives in Water

**Root cause**: The texture classifier (`road_classifier.py`) uses broad patterns like `"dirt"`, `"path"`, `"gravel"` that match coastal/riverbed/beach textures. The DBC "barren" check (confidence=0.5) catches underwater surfaces with no vegetation. The pipeline has **zero awareness of water** — ADT liquid data (MH2O/MCLQ sub-chunks) is never parsed.

**Current code** (`road_classifier.py:18-33`):
```python
ROAD_PATTERNS_MEDIUM = ["dirt", "path", "gravel"]  # too broad — matches riverbeds

EXCLUDE_PATTERNS = [...]  # missing: beach, shore, river, lake, ocean, ground, floor
```

**Current code** (`road_mask.py:54-61`): No confidence filtering — any `is_road=True` is included regardless of confidence score (0.5 or 1.0 treated identically).

### Problem 2: Duplicate Parallel Paths

**Root cause**: Road masks are wide and irregularly shaped. Skeletonization of thick blobs produces branches along edges. The morphological cleanup is too gentle:
- `disk(1)` closing/opening — only affects 1-pixel features (~0.5 yards)
- Roads in WoW are typically 6-15 yards wide = 12-30 pixels — skeleton of 30px-wide irregular blob has many internal branches

**Current code** (`graph_extractor.py:37-43`):
```python
def cleanup_mask(mask, min_size=20):
    kernel = disk(1)          # too small — doesn't smooth road edges
    cleaned = closing(mask, kernel)
    cleaned = opening(cleaned, kernel)
    cleaned = remove_small_objects(cleaned, max_size=min_size)  # 20 pixels = 5 sq yards, too small
    return cleaned
```

### Problem 3: Gaps in Continuous Roads

**Root cause**: At MCNK chunk boundaries (every 64 pixels = 33.33 yards), alpha maps from adjacent chunks may not perfectly overlap. The closing operation with `disk(1)` bridges only 1-pixel gaps. Real gaps can be 2-5 pixels (1-3 yards).

### Problem 4: Chaotic Noise in Forested Zones (Duskwood)

**Root cause**: `"dirt"` pattern matches ALL dirt floor textures — forest floors, fields, clearings. Duskwood uses textures like `Duskwood_Dirt01.blp` for general ground, not just roads. The exclude list has `"forest"` and `"leaves"` but most dirt textures don't contain these words.

**Example false matches**:
- `Tileset\Duskwood\Duskwood_Dirt01.blp` → `road_medium` (contains "dirt", no exclude match)
- `Tileset\Barrens\Barrens_Dirt01.blp` → `road_medium` (natural terrain, not road)
- `Tileset\Wetlands\Wetlands_DirtMud01.blp` → excluded by "mud" ✓ (but only because "mud" happens to be in name)

### Problem 5: Messy Intersections

**Root cause**: Wide intersection areas produce complex skeleton topology. Spur pruning at 3 iterations only removes 3-pixel branches (~1.5 yards). At a 20-pixel-wide intersection, skeleton branches can be 10+ pixels long. No graph-level post-processing exists to merge close nodes or simplify junction topology.

---

## Phase Plan

| Phase | Description | Impact |
|-------|-------------|--------|
| Phase 1 | Texture classifier hardening | Removes ~50% of false positives at source |
| Phase 2 | Water zone filtering (MH2O/MCLQ) | Eliminates all water/coast false positives |
| Phase 3 | Morphological mask improvements | Bridges gaps, removes thin artifacts |
| Phase 4 | Graph post-processing | Clean single-line paths, simple intersections |
| Phase 5 | Verification & tuning | Test on known problem areas |

---

## Phase 1: Texture Classifier Hardening

### 1.1: Restrict Medium-Confidence Patterns

The `"dirt"` pattern alone is far too broad. Change strategy: require compound matching.

**File**: `src/road_classifier.py`

**Current** `ROAD_PATTERNS_MEDIUM`:
```python
ROAD_PATTERNS_MEDIUM = ["dirt", "path", "gravel"]
```

**New approach** — split into two tiers:

```python
# Medium patterns that are ONLY road when combined with a road keyword
ROAD_PATTERNS_COMPOUND = [
    # "dirt" alone is ambiguous — must appear alongside road/path indicators
    # e.g., "DirtRoad" matches HIGH, but "Dirt01" should NOT match medium
]

# These patterns are road-like on their own (still medium confidence)
ROAD_PATTERNS_MEDIUM = [
    "path",       # "Path" almost always means a walking path/trail
    "gravel",     # "Gravel" is typically road surface, rarely natural terrain
    "trail",      # explicit trail
]
```

Remove `"dirt"` from `ROAD_PATTERNS_MEDIUM` entirely. Standalone `"dirt"` textures (e.g., `Duskwood_Dirt01.blp`) are natural terrain. Textures like `DirtRoad01.blp` already match HIGH via `"dirtroad"` and `"road"`.

If some `"dirt"` textures are genuine roads but don't contain "road"/"path", handle them via zone overrides in `road_textures.json` rather than a global pattern.

### 1.2: Expand Exclude Patterns

**File**: `src/road_classifier.py`

Add to `EXCLUDE_PATTERNS`:
```python
EXCLUDE_PATTERNS = [
    # existing
    "grass", "forest", "leaves", "rock", "cliff",
    "snow", "sand", "moss", "water", "lava", "root",
    "fern", "weed", "mud", "swamp", "marsh",
    "base", "scrub", "brush", "flower", "field",
    # new — water/coast
    "beach", "shore", "river", "lake", "ocean", "sea",
    "coral", "kelp", "algae", "underwater",
    # new — natural terrain
    "ground", "floor", "terrain", "earth",
    "farmland", "crop", "farm",
    "cave", "cavern", "mine",
    "rubble", "debris", "ruin",
]
```

### 1.3: Add Confidence Threshold to Mask Builder

**File**: `src/road_mask.py`

Currently ANY road classification (confidence >= 0.5) is included. Add a `min_confidence` parameter:

```python
def build_road_mask(
    adt, wdt_mphd_flags, threshold=64,
    config=None, dbc_data=None, zone_hint="",
    min_confidence=0.8,   # NEW — only include high-confidence roads
):
    ...
    is_road, confidence = is_road_layer(...)
    if not is_road or confidence < min_confidence:  # NEW check
        continue
    ...
```

**Default `min_confidence=0.8`**: Only `road_high` (1.0) and `road_medium + barren_dbc` (0.9) pass. Pure `road_medium` (0.6) and standalone barren (0.5) are excluded by default.

Add `--min-confidence` CLI parameter to `main.py`.

### 1.4: Zone Override Config Updates

**File**: `config/road_textures.json`

Add known problematic zones with explicit include/exclude:
```json
{
  "zone_overrides": {
    "Duskwood": {
      "exclude": ["Duskwood_Dirt01.blp", "Duskwood_Dirt02.blp"]
    },
    "Barrens": {
      "exclude": ["Barrens_Dirt01.blp", "Barrens_DirtCracked.blp"]
    },
    "Westfall": {
      "exclude": ["Westfall_Dirt01.blp", "Westfall_FarmDirt.blp"]
    }
  }
}
```

> **Note**: Exact texture filenames need verification by running `--dump-info` on tiles from these zones to see which textures actually exist. Add a `--dump-textures` mode that lists ALL unique textures across all tiles with their classification.

### 1.5: Add `--dump-textures` Diagnostic Mode

**File**: `src/main.py`

New CLI flag `--dump-textures` that processes all tiles and outputs a sorted list of every unique texture path, its classification, and how many chunks it appears in. Format:

```
road_high    (487 chunks) Tileset\Elwynn\ElwynnDirtRoad01.blp
road_medium  (312 chunks) Tileset\Elwynn\ElwynnGravel01.blp
not_road     (1205 chunks) Tileset\Elwynn\ElwynnGrass01.blp
```

This is essential for tuning the classifier — we need to see exactly what textures exist and how they classify.

---

## Phase 2: Water Zone Filtering (MH2O/MCLQ)

### 2.1: Parse MH2O Liquid Data from ADT

**File**: `src/adt_parser.py`

ADT chunks contain liquid (water, lava, slime) data in MH2O or legacy MCLQ sub-chunks. Parse this to create a water mask.

**MH2O structure** (per chunk):
- Located via MHDR offset at header byte 0x14 (ofsMH2O)
- Contains 256 entries (one per MCNK) of 24 bytes each:
  - `offset_instances` (uint32) — offset to instance data, 0 = no liquid
  - `layer_count` (uint32) — number of liquid layers
  - `offset_attributes` (uint32) — offset to attributes

- Each instance (24 bytes):
  - `liquid_type` (uint16)
  - `liquid_vertex_format` (uint16)
  - `min_height_level` (float)
  - `max_height_level` (float)
  - `x_offset` (uint8), `y_offset` (uint8), `width` (uint8), `height` (uint8) — sub-rect within 8x8 grid
  - `offset_exists_bitmap` (uint32) — which cells have liquid
  - `offset_vertex_data` (uint32) — height data

**Simplified approach**: We don't need the full liquid mesh — just need to know which MCNK chunks have liquid coverage. If a chunk has MH2O with `layer_count > 0` and `offset_instances != 0`, mark it as "has water".

Add to `McnkData`:
```python
@dataclass
class McnkData:
    ...
    has_liquid: bool = False        # NEW
    liquid_coverage: float = 0.0    # NEW — fraction of chunk covered by liquid (0.0-1.0)
```

### 2.2: Build Water Mask

**File**: `src/road_mask.py`

Create a 1024x1024 water mask alongside the road mask. For chunks with liquid, expand the liquid coverage to 64x64 pixels using the MH2O exists-bitmap (8x8 grid, each cell = 8x8 pixels).

```python
def build_water_mask(adt: AdtData) -> np.ndarray:
    """Build 1024x1024 boolean mask where True = water/lava/slime."""
    mask = np.zeros((1024, 1024), dtype=bool)
    for chunk in adt.chunks:
        if chunk.has_liquid:
            r0 = chunk.index_y * 64
            c0 = chunk.index_x * 64
            # Use liquid_bitmap if available, else mark entire chunk
            mask[r0:r0+64, c0:c0+64] = True  # simplified — refine with bitmap
    return mask
```

### 2.3: Subtract Water from Road Mask

**File**: `src/road_mask.py` or `src/graph_extractor.py`

After building road mask, subtract water mask:

```python
road_mask = build_road_mask(adt, wdt_mphd_flags, ...)
water_mask = build_water_mask(adt)
road_mask = road_mask & ~water_mask
```

This eliminates ALL road detections in water areas — rivers, lakes, ocean, lava.

### 2.4: Coastline Buffer

Add an optional erosion buffer around water zones (e.g., 5 pixels = ~2.5 yards) to remove false positive road detections at the water's edge:

```python
from skimage.morphology import binary_dilation, disk

water_buffer = binary_dilation(water_mask, disk(5))
road_mask = road_mask & ~water_buffer
```

This handles cases where road textures bleed into the water transition zone.

---

## Phase 3: Morphological Mask Improvements

### 3.1: Increase Closing Radius

**File**: `src/graph_extractor.py`, function `cleanup_mask()`

Current `disk(1)` bridges only 1-pixel gaps. Increase to `disk(3)` to bridge 3-pixel gaps (~1.5 yards), which covers most chunk boundary discontinuities:

```python
def cleanup_mask(mask, min_size=200, close_radius=3, open_radius=2):
    # Step 1: Close small gaps (bridge chunk boundaries)
    closed = closing(mask, disk(close_radius))

    # Step 2: Remove thin artifacts (clean edges)
    cleaned = opening(closed, disk(open_radius))

    # Step 3: Remove small isolated regions
    cleaned = remove_small_objects(cleaned, min_size=min_size)

    return cleaned
```

### 3.2: Increase Minimum Object Size

Change `min_size` from **20** to **200** pixels.

- 20 pixels = ~5 square yards — too small, keeps noise
- 200 pixels = ~52 square yards — removes isolated dirt patches while keeping genuine road segments (a 10-yard road segment at 10px width = 100px area minimum)

Add CLI parameter `--min-road-size` (default 200).

### 3.3: Add Width-Based Filtering

After closing/opening but before skeletonization, filter by local width. True roads should be at least 3 pixels wide (~1.5 yards). Very thin (1-2px) linear features are noise:

```python
from scipy.ndimage import distance_transform_edt

def filter_by_width(mask, min_width_px=3):
    """Remove regions thinner than min_width_px."""
    dist = distance_transform_edt(mask)
    # Keep only pixels where distance to boundary >= min_width_px / 2
    wide_enough = dist >= (min_width_px / 2.0)
    # Reconstruct: dilate the "wide enough" skeleton back
    result = binary_dilation(wide_enough, disk(min_width_px // 2))
    return mask & result
```

### 3.4: Add CLI Parameters

**File**: `src/main.py`

New parameters:
```
--min-confidence  float  0.8   Minimum classification confidence to include
--close-radius    int    3     Morphological closing radius (pixels)
--open-radius     int    2     Morphological opening radius (pixels)
--min-road-size   int    200   Minimum connected component size (pixels)
--min-road-width  int    3     Minimum road width (pixels)
--water-buffer    int    5     Water zone erosion buffer (pixels)
```

---

## Phase 4: Graph Post-Processing

After graph extraction, apply cleanup operations to produce clean single-line paths with simple intersections.

### 4.1: Increase Spur Pruning

**File**: `src/graph_extractor.py`

Change default `prune_iterations` from **3** to **15**.

- 3 iterations removes branches up to ~1.5 yards — too conservative
- 15 iterations removes branches up to ~7.5 yards — covers most skeleton artifacts from road edges
- Real intersecting roads are longer than 7.5 yards, so they survive

### 4.2: Merge Close Nodes (Intersection Simplification)

**New function** in `src/graph_extractor.py`:

After skeleton-to-graph, merge nodes that are within a distance threshold. This collapses messy intersection clusters into single junction points.

```python
def merge_close_nodes(graph: nx.Graph, distance_threshold: float = 8.0) -> nx.Graph:
    """Merge graph nodes that are within distance_threshold yards of each other.

    Uses union-find to cluster close nodes, then replaces each cluster
    with a single node at the centroid position. Reconnects all edges.

    This collapses noisy intersection clusters into single junction nodes.

    Parameters:
        graph: input graph with world_x, world_y on nodes
        distance_threshold: max distance in yards to merge (default 8.0)
            - WoW roads are 6-15 yards wide
            - 8 yards merges nodes on the same road width
            - Won't merge nodes on different parallel roads (>15 yards apart)
    """
```

Algorithm:
1. Build spatial index (same cell-based approach as `merge_tile_graphs_spatial`)
2. Find all node pairs within `distance_threshold` (from SAME tile, not cross-tile)
3. Union-find to cluster
4. Replace each cluster with single node at centroid
5. Reconnect edges, remove self-loops and duplicate edges

### 4.3: Remove Short Isolated Components

**New function** in `src/graph_extractor.py`:

Remove connected components shorter than a minimum total length. Isolated waypoint clusters with < N yards of total edges are noise, not roads.

```python
def remove_short_components(graph: nx.Graph, min_length: float = 30.0) -> nx.Graph:
    """Remove connected components with total edge length < min_length yards.

    A real road segment is at least 30 yards long. Shorter components are:
    - Dirt patches misclassified as roads
    - Artifact clusters from texture bleed
    - Isolated intersection noise

    Parameters:
        min_length: minimum total edge length in yards (default 30.0)
    """
    components = list(nx.connected_components(graph))
    nodes_to_remove = set()

    for component in components:
        subgraph = graph.subgraph(component)
        total_length = sum(
            edata.get("cost", 0.0)
            for _, _, edata in subgraph.edges(data=True)
        )
        if total_length < min_length:
            nodes_to_remove.update(component)

    graph.remove_nodes_from(nodes_to_remove)
    return graph
```

### 4.4: Collapse Parallel Edges

**New function** in `src/graph_extractor.py`:

Detect and merge parallel paths — where two edges between similar pairs of nodes run alongside each other within a road width.

```python
def collapse_parallel_paths(graph: nx.Graph, max_separation: float = 10.0) -> nx.Graph:
    """Detect parallel edges running within max_separation yards and merge them.

    For degree-2 chains (node-edge-node-edge-...-node), if two chains have:
    - Similar start/end regions (endpoints within max_separation)
    - Similar total length (within 20%)
    - Midpoints within max_separation

    Then merge them into a single chain at the average position.
    """
```

This is the most complex operation. Simplified approach:
1. Find all degree-2 node chains (paths with no branching)
2. For each pair of chains, check if they are "parallel" (endpoints and midpoints close)
3. Merge parallel chains: keep one, average positions, reconnect

### 4.5: Degree-2 Node Elimination

**New function** in `src/graph_extractor.py`:

After merging close nodes, some junction nodes become pass-through (degree 2). Eliminate them by merging their two edges into one:

```python
def eliminate_degree2_nodes(graph: nx.Graph) -> nx.Graph:
    """Remove degree-2 nodes by merging their two edges into one.

    A degree-2 node in the middle of a road adds no topological information.
    Replace A-B-C (where B has degree 2) with A-C.

    Preserves the edge cost as sum of the two original edges.
    """
```

### 4.6: Updated Pipeline

**File**: `src/graph_extractor.py`, function `extract_road_graph()`

```python
def extract_road_graph(
    mask, origin_x, origin_y,
    min_size=200,           # was 20
    close_radius=3,         # NEW, was disk(1)
    open_radius=2,          # NEW, was disk(1)
    min_road_width=3,       # NEW
    prune_iterations=15,    # was 3
    dp_epsilon=2.0,
    merge_node_distance=8.0,  # NEW
    min_component_length=30.0,  # NEW
):
    # 1. Cleanup (improved)
    cleaned = cleanup_mask(mask, min_size=min_size,
                           close_radius=close_radius, open_radius=open_radius)

    # 2. Width filter (NEW)
    if min_road_width > 1:
        cleaned = filter_by_width(cleaned, min_width_px=min_road_width)

    # 3. Skeletonize + prune (more aggressive)
    skeleton = extract_skeleton(cleaned, prune_iterations=prune_iterations)

    if not skeleton.any():
        return nx.Graph()

    # 4. Skeleton → graph
    graph = skeleton_to_graph(skeleton)
    if graph.number_of_nodes() == 0:
        return graph

    # 5. World coordinates
    graph = assign_world_coords(graph, origin_x, origin_y)

    # 6. Simplify edges
    graph = simplify_edges(graph, epsilon=dp_epsilon)

    # 7. Merge close nodes (NEW — intersection cleanup)
    graph = merge_close_nodes(graph, distance_threshold=merge_node_distance)

    # 8. Remove short isolated components (NEW)
    graph = remove_short_components(graph, min_length=min_component_length)

    # 9. Collapse parallel paths (NEW)
    graph = collapse_parallel_paths(graph)

    # 10. Eliminate pass-through nodes (NEW)
    graph = eliminate_degree2_nodes(graph)

    return graph
```

---

## Phase 5: Verification & Tuning

### 5.1: Test Tiles

Test on the following known problem areas and verify improvements:

| Area | Tiles | Expected Result |
|------|-------|-----------------|
| Westfall coast (screenshot 1) | ~51,30 | No waypoints in water |
| Elwynn T-junction (screenshot 2) | ~50,30 | Clean T with 1 junction node, 3 edges |
| Duskwood (screenshots 3-4) | ~51,32 | Only road waypoints, no forest floor noise |
| Elwynn T-junction detail (screenshot 5) | ~50,30 | Single centerline per road, clean junction |
| Goldshire | 31,48 | Known road shapes, continuous paths |
| Stormwind approach | 31,49 | Wide paved road = single centerline |

### 5.2: Before/After Metrics

For each test tile, compare:
- Node count (expect 70-80% reduction)
- Edge count (expect 60-70% reduction)
- Connected components (expect fewer, larger components)
- Visual inspection in map-editor

### 5.3: Parameter Tuning Guide

If issues persist after implementation:

| Problem | Tune |
|---------|------|
| Still too many false roads | Raise `--min-confidence` to 0.9 or 1.0 |
| Some real roads missing | Lower `--threshold` from 64 to 48, or add to zone includes |
| Roads still have gaps | Increase `--close-radius` to 5 |
| Too many noise clusters | Raise `--min-road-size` to 500 |
| Parallel paths persist | Lower `merge_node_distance` to 5, or increase to 12 |
| Intersections still messy | Raise `merge_node_distance` to 12-15 |
| Short side-roads removed | Lower `--min-component-length` to 15 |

---

## Files to Modify

| File | Changes |
|------|---------|
| `src/road_classifier.py` | Remove `"dirt"` from medium patterns, add exclude patterns, add `"trail"` |
| `src/road_mask.py` | Add `min_confidence` parameter, add `build_water_mask()`, subtract water from roads |
| `src/adt_parser.py` | Parse MH2O/MCLQ liquid data, add `has_liquid`/`liquid_coverage` to `McnkData` |
| `src/graph_extractor.py` | Larger morphology, width filter, aggressive pruning, node merging, component filtering, parallel collapse, degree-2 elimination |
| `src/main.py` | Add new CLI parameters, integrate water mask, add `--dump-textures` mode |
| `config/road_textures.json` | Add zone overrides for Duskwood, Barrens, Westfall |

**No new files needed.** All changes are modifications to existing pipeline modules.

---

## Implementation Order

```
Phase 1: Texture Classifier Hardening
    ├── 1.1: Remove "dirt" from medium patterns
    ├── 1.2: Expand exclude patterns
    ├── 1.3: Add min_confidence to road_mask.py
    ├── 1.4: Update road_textures.json zone overrides
    └── 1.5: Add --dump-textures diagnostic mode
         ↓
Phase 2: Water Zone Filtering
    ├── 2.1: Parse MH2O liquid data in adt_parser.py
    ├── 2.2: Build water mask
    ├── 2.3: Subtract water from road mask
    └── 2.4: Coastline buffer erosion
         ↓
Phase 3: Morphological Mask Improvements
    ├── 3.1: Increase closing radius (1 → 3)
    ├── 3.2: Increase min object size (20 → 200)
    ├── 3.3: Add width-based filtering
    └── 3.4: Add new CLI parameters
         ↓
Phase 4: Graph Post-Processing
    ├── 4.1: Increase spur pruning (3 → 15 iterations)
    ├── 4.2: Merge close nodes (intersection simplification)
    ├── 4.3: Remove short isolated components
    ├── 4.4: Collapse parallel edges
    ├── 4.5: Degree-2 node elimination
    └── 4.6: Wire into extract_road_graph() pipeline
         ↓
Phase 5: Verification & Tuning
    ├── 5.1: Test on known problem tiles
    ├── 5.2: Compare before/after metrics
    └── 5.3: Parameter tuning if needed
```

**Recommended execution**: Phases 1-2 first (removes majority of false positives), then Phase 3 (cleaner masks), then Phase 4 (clean graph). Each phase independently improves quality and can be tested incrementally.

---

## Key Technical Context

### ADT File Format (WoW 3.3.5a)
- Each ADT = 16x16 grid of MCNK chunks, each chunk = 33.33 yards = 64x64 alpha pixels
- Full tile = 1024x1024 pixels = 533.33 yards
- Pixel size = ~0.52 yards
- Alpha maps: 4-bit packed (Eastern Kingdoms), 8-bit raw (Northrend), or RLE compressed (per-layer flag)
- Textures: up to 4 layers per chunk, layer 0 = base (no alpha), layers 1-3 = blend with alpha

### Coordinate System
- WoW: X+ = north, Y+ = west, Z+ = up
- ADT filename: `{Map}_{A}_{B}.adt` where A maps to wowY axis (tileY), B maps to wowX axis (tileX)
- Tile origin: `wowX = (32 - B) * 533.33`, `wowY = (32 - A) * 533.33`
- Pixel → world: `world_x = origin_x - row * 0.520833`, `world_y = origin_y - col * 0.520833`

### MH2O Liquid Format (for Phase 2)
- MH2O is a top-level ADT chunk (not per-MCNK sub-chunk)
- Located via MHDR offset at header position 0x14
- Contains 256 SLiquidChunk entries (one per MCNK), each 24 bytes:
  ```c
  struct SLiquidChunk {
      uint32_t offset_instances;  // 0 = no liquid
      uint32_t layer_count;
      uint32_t offset_attributes;
  };
  ```
- Each SLiquidInstance (24 bytes):
  ```c
  struct SLiquidInstance {
      uint16_t liquid_type;     // → LiquidType.dbc
      uint16_t liquid_vertex_format;
      float    min_height;
      float    max_height;
      uint8_t  x_offset;       // sub-rect in 8x8 grid
      uint8_t  y_offset;
      uint8_t  width;
      uint8_t  height;
      uint32_t offset_exists_bitmap;  // bit per cell
      uint32_t offset_vertex_data;
  };
  ```
- Reference: wowdev.wiki/ADT/v18#MH2O

### Current Pipeline Parameters (defaults to change)
| Parameter | Current | Proposed | Rationale |
|-----------|---------|----------|-----------|
| min_confidence | none (any) | 0.8 | Exclude low-confidence classifications |
| ROAD_PATTERNS_MEDIUM | `["dirt","path","gravel"]` | `["path","gravel","trail"]` | Remove "dirt" (too broad) |
| close radius | disk(1) | disk(3) | Bridge chunk boundary gaps |
| open radius | disk(1) | disk(2) | Remove thin artifacts |
| min_size | 20 | 200 | Remove small noise clusters |
| prune_iterations | 3 | 15 | Remove longer skeleton branches |
| merge_node_distance | none | 8.0 yards | Simplify intersections |
| min_component_length | none | 30.0 yards | Remove isolated noise |

### Dependencies
- All existing: `numpy`, `scikit-image`, `scipy`, `networkx`, `sknw`, `rdp`, `Pillow`
- No new dependencies required
- MH2O parsing uses only `struct` (stdlib)
