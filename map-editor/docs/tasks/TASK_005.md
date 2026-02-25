# TASK-005: Road Network Extraction from ADT Terrain Files (Python Prototype)

**Status**: COMPLETED
**Created**: 2026-02-25
**Completed**: 2026-02-25

---

## Overview

Extract road coordinates from WoW 3.3.5a ADT terrain files by analyzing texture layers and alpha maps, then build a navigable road network graph. Roads in WoW are not discrete objects — they are alpha-blended texture layers on terrain chunks. This task creates a Python prototyping pipeline to parse ADT files, classify road textures, build binary road masks, and extract a graph of road centerlines with world coordinates.

**Goal**: Produce a JSON file containing a road network graph (nodes with WoW world coordinates, edges with polyline geometry) for any continent/zone, suitable for import into the map editor's world graph system.

**Location**: Scripts in `map-editor/docs/scripts/road_extraction/` within the `wotlk-utils` repository.

**Prerequisites**:
- WoW 3.3.5a client Data directory (MPQ archives: `common.MPQ`, `expansion.MPQ`, `lichking.MPQ`, patches)
- Python 3.10+ with `uv` package manager
- StormLib shared library (`.dll`/`.so`) for MPQ access

**Research Documents** (in `map-editor/docs/researches/roads/`):
1. `compass_artifact_wf-...text_markdown.md` — ADT binary format, MCAL formats, coordinate mapping, road identification, pipeline overview (**primary reference, most accurate**)
2. `Извлечение дорожных текстур из ADT.md` — Detailed analysis of ADT architecture, MCAL decode algorithms, GroundEffectTexture.dbc chain, skeletonization theory
3. `Построение дорожной сети...на Python.md` — Texture classification strategies, script architecture proposals, library recommendations

> **WARNING**: Document 3 contains significant errors. See [Critical Implementation Notes](#critical-implementation-notes) for details. Always cross-reference against document 1 and wowdev.wiki.

---

## Phase Status Summary

| Phase | Description | Status |
|-------|-------------|--------|
| Phase 1 | Project Setup + WDT/ADT Parser | COMPLETED |
| Phase 2 | Road Texture Classification | COMPLETED |
| Phase 3 | Road Visualization + Multi-Tile Stitching | COMPLETED |
| Phase 4 | Road Graph Extraction | COMPLETED |
| Phase 5 | Full-Map Extraction + Map-Editor Integration | COMPLETED |

---

## Phase 1: Project Setup + WDT/ADT Parser

### Goal

Create a `uv`-managed Python project that can open MPQ archives, parse WDT files (to determine alpha map format), and parse ADT files (MCNK/MTEX/MCLY/MCAL) into structured data. Output: decoded 64x64 alpha maps for every texture layer of every chunk.

### 1.1: Project Scaffold

```
map-editor/docs/scripts/road_extraction/
    pyproject.toml          # uv project config
    README.md               # usage instructions
    src/
        __init__.py
        mpq_reader.py       # MPQ archive wrapper
        wdt_parser.py       # WDT parser (MPHD flags)
        adt_parser.py       # ADT parser (MCNK, MTEX, MCLY, MCAL)
        mcal_decoder.py     # Alpha map decode (3 formats)
        coord_utils.py      # Pixel-to-world coordinate mapping
        road_classifier.py  # Texture classification (Phase 2)
        road_mask.py        # Binary mask builder (Phase 2)
        visualizer.py       # Tile rendering (Phase 3)
        stitcher.py         # Multi-tile assembly (Phase 3)
        graph_extractor.py  # Skeleton -> graph (Phase 4)
    config/
        road_textures.json  # Per-zone road texture overrides
    output/                 # Generated masks, graphs, images
    tests/
        test_mcal_decoder.py
        test_coord_utils.py
        test_adt_parser.py
```

**pyproject.toml** dependencies:

```toml
[project]
name = "wow-road-extraction"
version = "0.1.0"
requires-python = ">=3.10"
dependencies = [
    "numpy>=1.24",
    "Pillow>=10.0",
    "scikit-image>=0.21",
    "scipy>=1.11",
    "networkx>=3.1",
    "sknw>=0.14",
    "rdp>=0.8",
    "matplotlib>=3.7",
]
```

MPQ access requires a StormLib-based Python binding. Options:
- `python-mpq` by jleclanche (`pip install mpq`) — StormLib-based, recommended
- `stormlib-python` / `pyStormLib` — ctypes/cffi wrapper around prebuilt StormLib
- Direct ctypes wrapper around StormLib DLL (most control, see `mpq_reader.py`)

### 1.2: WDT Parser — Alpha Format Determination

The WDT file determines whether alpha maps are 4-bit or 8-bit for the entire map. This is **critical** — parsing alpha data with the wrong format produces garbage.

**File location**: `World\Maps\{MapInternalName}\{MapInternalName}.wdt`

**Key chunk**: MPHD (Map Header) — contains `flags` field at offset 0:

```python
# wdt_parser.py — key signature
def parse_wdt(data: bytes) -> WdtInfo:
    """Parse WDT file, extract MPHD flags and MAIN tile existence grid."""
    # Returns: WdtInfo(mphd_flags, tile_exists[64][64])
```

**MPHD flags relevant to alpha format**:
- Bit 2 (`0x0004`): `adt_has_big_alpha` — alpha maps use 8-bit format (4096 bytes)
- Bit 3 (`0x0008`): `adt_has_height_texturing` — implies big alpha

**Known map configurations** (WoW 3.3.5a):

| Map | MapID | MPHD flag 0x4 | Default alpha format |
|-----|-------|---------------|---------------------|
| Eastern Kingdoms (Azeroth) | 0 | NOT set | 4-bit packed (2048 bytes) |
| Kalimdor | 1 | NOT set | 4-bit packed (2048 bytes) |
| Outland (Expansion01) | 530 | Varies | Check WDT |
| Northrend | 571 | SET | 8-bit raw (4096 bytes) |

### 1.3: ADT Parser

**File location**: `World\Maps\{Name}\{Name}_{TileX}_{TileY}.adt`

ADT files in WoW 3.3.5a are monolithic (no `_tex0`/`_obj0` split — that's Cataclysm+).

**Parsing order**:

1. **MVER** — Version check (must be 18 for WotLK)
2. **MHDR** — Header with offsets to other chunks. All MHDR offsets are **relative to MHDR data start** (after the 8-byte IFF header)
3. **MCIN** — 256 entries x 16 bytes. Offsets are **absolute from file start**. Row-major order: entry 0 = chunk(0,0) at NW corner
4. **MTEX** — Null-separated texture filename strings (full MPQ paths)
5. **MCNK[256]** — 16x16 grid of terrain chunks, each containing sub-chunks

**Chunk tag byte order**: All IFF tags are stored reversed in file. `MCNK` appears as `KNCM` bytes. Compare as `memcmp(ptr, "KNCM", 4)` or read uint32 LE and match reversed constant.

```python
# adt_parser.py — key signatures
def parse_adt(data: bytes) -> AdtData:
    """Parse complete ADT file into structured data."""
    # Returns: AdtData(mtex_list, chunks[256])

def parse_mcnk(data: bytes, offset: int) -> McnkData:
    """Parse single MCNK chunk at given file offset."""
    # Returns: McnkData(position, index_x, index_y, n_layers, layers[], alpha_data)
```

**MCNK header** (128 bytes at offset 0x08 from MCNK IFF tag):

Key fields:
- `0x0C`: `nLayers` (uint32) — texture layer count (1-4)
- `0x1C`: `ofsMCLY` (uint32) — offset to MCLY sub-chunk, **relative to MCNK IFF tag start**
- `0x24`: `ofsMCAL` (uint32) — offset to MCAL sub-chunk, **relative to MCNK IFF tag start**
- `0x28`: `sizeAlpha` (uint32) — actual MCAL data size (**use this, not the IFF chunk size**)
- `0x68`: `position[3]` (float x3) — stored as **(Y, Z, X)** in WoW world coords. So: `position[0]=wowY, position[1]=wowZ, position[2]=wowX`. This is the **NW corner** of the chunk

**Sub-chunk offset rule**: `file_position = MCNK_tag_position + offset_value`. For example, `ofsMCVT=0x88` means MCVT starts 0x88 bytes after the MCNK tag = 8 (IFF header) + 128 (MCNK header) = right after the header.

### 1.4: MCLY — Texture Layer Definitions

Each MCLY entry is 16 bytes:

```c
struct MCLYEntry {
    uint32_t textureId;      // index into MTEX string list
    uint32_t flags;          // see flag table below
    uint32_t offsetInMCAL;   // offset of this layer's alpha data within MCAL data
    int32_t  effectId;       // → GroundEffectTexture.dbc (for road classification)
};
```

**MCLY flags**:

| Bit | Mask | Name | Description |
|-----|------|------|-------------|
| 0-2 | 0x007 | animation_rotation | 45 degrees per increment |
| 3-5 | 0x038 | animation_speed | Higher = faster |
| 6 | 0x040 | animation_enabled | Animate this texture |
| 7 | 0x080 | overbright | Glow effect (lava) |
| **8** | **0x100** | **use_alpha_map** | **Set on layers 1-3; never on layer 0** |
| **9** | **0x200** | **alpha_map_compressed** | **This layer's alpha data is RLE-compressed** |
| 10 | 0x400 | use_cube_map_reflection | Ice surfaces (WotLK+) |

**Fundamental rule**: Layer 0 is the base texture — 100% opacity, no alpha map. Road textures are ALWAYS on layers 1-3.

### 1.5: MCAL Decoder — Three Formats

The alpha format is determined by combining WDT-level and per-layer flags:

```python
# mcal_decoder.py — format selection
def get_alpha_format(wdt_mphd_flags: int, mcly_flags: int) -> str:
    if mcly_flags & 0x200:                  # per-layer compression flag
        return 'compressed_rle'             # always decompresses to 4096 bytes (8-bit)
    elif wdt_mphd_flags & 0x0004:           # WDT big_alpha flag
        return 'uncompressed_8bit'          # 4096 bytes raw
    else:
        return 'uncompressed_4bit'          # 2048 bytes packed
```

#### Format 1: 4-bit packed (2048 bytes)

Used on old-world continents (Eastern Kingdoms, Kalimdor) without `adt_has_big_alpha`. Each byte contains two 4-bit values, **low nibble first**:

```python
def decode_4bit_alpha(data: bytes) -> np.ndarray:  # 2048 bytes → 64x64 uint8
    alpha = np.zeros((64, 64), dtype=np.uint8)
    for row in range(64):
        for col in range(64):
            linear = row * 64 + col
            byte_idx = linear // 2
            if linear % 2 == 0:
                val = data[byte_idx] & 0x0F         # low nibble first
            else:
                val = (data[byte_idx] >> 4) & 0x0F   # high nibble second
            alpha[row, col] = val | (val << 4)        # expand 0-15 → 0-255
    return alpha
```

#### Format 2: 8-bit uncompressed (4096 bytes)

Used on Northrend and maps with `adt_has_big_alpha`:

```python
def decode_8bit_alpha(data: bytes) -> np.ndarray:  # 4096 bytes → 64x64 uint8
    return np.frombuffer(data, dtype=np.uint8).reshape(64, 64)
```

#### Format 3: RLE compressed (variable size → 4096 bytes)

Activated when MCLY flag `0x200` is set. Always decompresses to 4096 bytes (8-bit). Two modes selected by high bit of control byte:

```python
def decompress_alpha_rle(data: bytes) -> np.ndarray:  # variable → 64x64 uint8
    output = bytearray(4096)
    in_pos = 0
    out_pos = 0
    while out_pos < 4096:
        control = data[in_pos]; in_pos += 1
        count = control & 0x7F
        if control & 0x80:                          # FILL mode: repeat one value
            value = data[in_pos]; in_pos += 1
            for _ in range(count):
                if out_pos >= 4096: break
                output[out_pos] = value; out_pos += 1
        else:                                       # COPY mode: literal bytes
            for _ in range(count):
                if out_pos >= 4096: break
                output[out_pos] = data[in_pos]; in_pos += 1; out_pos += 1
    return np.frombuffer(bytes(output), dtype=np.uint8).reshape(64, 64)
```

**Note**: Some Blizzard ADTs contain buggy compressed data that would decompress to >4096 bytes. Always cap at exactly 4096 output bytes (as Noggit does).

### 1.6: Coordinate Utilities

```python
# coord_utils.py — key constants and functions
TILE_SIZE  = 533.33333       # yards per ADT tile (= 1600/3)
CHUNK_SIZE = TILE_SIZE / 16  # 33.33333 yards per MCNK chunk (= 100/3)
PIXEL_SIZE = CHUNK_SIZE / 64 # ~0.520833 yards per alpha pixel (= 25/48)
MAP_ORIGIN = 32 * TILE_SIZE  # 17066.66666 (NW corner of full 64x64 grid)

def pixel_to_world(mcnk_position_x: float, mcnk_position_y: float,
                   pixel_row: int, pixel_col: int) -> tuple[float, float]:
    """Convert alpha map pixel (row, col) to WoW world (X, Y).

    MCNK position is the NW corner of the chunk.
    Pixel (0,0) = NW corner. Row increases southward (decreasing X).
    Column increases eastward (decreasing Y).
    """
    world_x = mcnk_position_x - pixel_row * PIXEL_SIZE
    world_y = mcnk_position_y - pixel_col * PIXEL_SIZE
    return world_x, world_y
```

**Pixel orientation**: `(0,0)` is the **NW corner** of the chunk. Row index increases southward (decreasing world X). Column index increases eastward (decreasing world Y). Adjacent chunks abut directly — no shared border pixels, no overlap.

### 1.7: Phase 1 Verification

- [ ] WDT parser correctly reads MPHD flags (verify: Eastern Kingdoms has no `0x4`, Northrend has `0x4`)
- [ ] ADT parser reads all 256 MCNK chunks with correct positions
- [ ] MTEX list extraction matches known texture paths for Elwynn Forest tiles
- [ ] MCLY layer count and flags match expected values (layer 0: no alpha, layers 1-3: have alpha)
- [ ] 4-bit alpha decode: output range 0-255, 2048 bytes input → 64x64 array
- [ ] 8-bit alpha decode: trivial reshape, 4096 bytes → 64x64 array
- [ ] RLE decompress: variable input → exactly 4096 output bytes → 64x64 array
- [ ] Coordinate mapping: pixel (0,0) maps to chunk NW corner; pixel (63,63) maps to SE corner ~33.33 yards away

**Test tile**: Elwynn Forest — `World\Maps\Azeroth\Azeroth_31_48.adt` (tileX=31, tileY=48, Goldshire area). Use 4-bit alpha format (Eastern Kingdoms, no big_alpha). Verify decoded alpha values are non-zero where roads visually exist.

---

## Phase 2: Road Texture Classification

### Goal

Identify which texture layers represent roads vs. natural terrain (grass, rock, etc.). Output: for each MCNK chunk, a set of layer indices that are road layers.

### 2.1: Classification Strategy

There is **no MCLY flag, MCNK flag, or ADT-level marker** that labels a texture as "road." Classification requires examining the texture identity through two complementary approaches.

#### Approach A: Filename Pattern Matching (Primary)

Terrain textures follow predictable naming in MTEX paths:

```python
ROAD_PATTERNS_HIGH = [          # high confidence — always road
    'road', 'dirtroad', 'cobble', 'cobblestone',
    'flagstone', 'paving', 'pavement',
]
ROAD_PATTERNS_MEDIUM = [        # medium confidence — may include non-road uses
    'dirt', 'path', 'gravel',
]
EXCLUDE_PATTERNS = [            # exclude even if medium-confidence pattern matches
    'grass', 'forest', 'leaves', 'rock', 'cliff',
    'snow', 'sand', 'moss', 'water', 'lava', 'root',
]

def classify_texture(mtex_path: str) -> str:
    name = mtex_path.lower()
    if any(p in name for p in ROAD_PATTERNS_HIGH):
        return 'road_high'
    if any(p in name for p in ROAD_PATTERNS_MEDIUM):
        if not any(e in name for e in EXCLUDE_PATTERNS):
            return 'road_medium'
    return 'not_road'
```

**Examples** (Elwynn Forest):
- `Tileset\Elwynn\ElwynnDirtRoad01.blp` → `road_high` (contains "road")
- `Tileset\Elwynn\ElwynnDirt01.blp` → `road_medium` (contains "dirt", no exclude)
- `Tileset\Elwynn\ElwynnGrass01.blp` → `not_road` (excluded by "grass")
- `Tileset\Generic\GenericRoad01.blp` → `road_high`

#### Approach B: GroundEffectTexture.dbc Chain (Supplementary)

Each MCLY layer has an `effectId` field (int32 at offset 0x0C) linking into the DBC chain:

```
MCLY.effectId → GroundEffectTexture.dbc (m_ID)
    → m_doodadId[4]: detail doodad types (grass, flowers, rocks)
    → m_density: doodad spawn density
    → m_sound: → TerrainType.dbc (m_TerrainID) → m_TerrainDesc
```

**Key insight**: Road surfaces have **all 4 doodadId values = -1** (no grass/flora generation). This is a reliable marker — roads are intentionally kept clear of procedural vegetation.

**TerrainType.dbc** defines surface categories:

| TerrainID | Description | Road relevance |
|-----------|-------------|----------------|
| 0 | Dirt | Dirt roads + natural dirt terrain |
| 2 | Stone | Cobblestone roads + natural rock |
| 3 | Snow | Not road |
| 5 | Grass | Not road |

There is **no explicit "Road" terrain type**. Roads are classified as Dirt (0) or Stone (2). The DBC chain alone cannot distinguish a dirt road from natural dirt. Cross-referencing with filename matching yields the highest confidence.

#### Combined Strategy

```python
def is_road_layer(mtex_path: str, effect_id: int, dbc_data: dict) -> tuple[bool, float]:
    """Returns (is_road, confidence) where confidence is 0.0-1.0."""
    fname_class = classify_texture(mtex_path)

    # DBC check: all doodadId == -1 means no vegetation → likely road
    dbc_is_barren = False
    if effect_id > 0 and effect_id in dbc_data:
        rec = dbc_data[effect_id]
        dbc_is_barren = all(d == -1 or d == 0xFFFFFFFF for d in rec.doodad_ids)

    if fname_class == 'road_high':
        return True, 1.0                           # definite road
    if fname_class == 'road_medium' and dbc_is_barren:
        return True, 0.9                           # dirt + no vegetation = road
    if fname_class == 'road_medium':
        return True, 0.6                           # probably road but ambiguous
    if dbc_is_barren and fname_class != 'not_road':
        return True, 0.5                           # barren surface, unknown name
    return False, 0.0
```

### 2.2: Binary Road Mask Builder

For each ADT tile, produce a 1024x1024 binary mask (16 chunks x 64 pixels = 1024):

```python
def build_road_mask(adt: AdtData, wdt_flags: int, dbc_data: dict,
                    threshold: int = 64) -> np.ndarray:
    """Build 1024x1024 binary road mask for one ADT tile.

    threshold: alpha value cutoff (0-255). Default 64 (25%).
    """
    mask = np.zeros((1024, 1024), dtype=bool)
    for chunk_row in range(16):
        for chunk_col in range(16):
            chunk = adt.chunks[chunk_row * 16 + chunk_col]
            for layer_idx in range(1, chunk.n_layers):    # skip layer 0
                layer = chunk.layers[layer_idx]
                tex_path = adt.mtex_list[layer.texture_id]

                is_road, confidence = is_road_layer(tex_path, layer.effect_id, dbc_data)
                if not is_road:
                    continue

                alpha = decode_alpha(chunk.alpha_data, layer, wdt_flags)  # 64x64
                r0 = chunk_row * 64
                c0 = chunk_col * 64
                mask[r0:r0+64, c0:c0+64] |= (alpha > threshold)
    return mask
```

**Threshold selection**:

| Threshold | Behavior |
|-----------|----------|
| 30-50 (12-20%) | Very inclusive; catches faint edges, may include spurious bleed |
| **64 (25%)** | **Recommended default** — captures visually apparent road area while maintaining connectivity |
| 128 (50%) | Only where road texture is dominant; roads appear narrow, connectivity may break at transitions |
| 192 (75%) | Too strict; roads become disconnected fragments |

Start with **64**. Road textures at chunk boundaries often don't reach 50% alpha due to blending. Morphological cleanup + skeletonization find the correct centerline regardless of initial road width.

### 2.3: Per-Zone Configuration

Store per-zone texture overrides in `config/road_textures.json`:

```json
{
    "global_patterns_high": ["road", "cobble", "cobblestone", "flagstone", "paving"],
    "global_patterns_medium": ["dirt", "path", "gravel"],
    "global_exclude": ["grass", "forest", "leaves", "rock", "cliff", "snow", "sand", "lava"],
    "zone_overrides": {
        "Elwynn": {
            "include": ["ElwynnDirt01.blp"],
            "exclude": ["ElwynnDirtCliff.blp"]
        },
        "DunMorogh": {
            "include": ["DunMoroghSnowTrack.blp", "DunMoroghSnowDirt.blp"]
        }
    }
}
```

### 2.4: Phase 2 Verification

- [ ] Classifier correctly labels `*Road*`, `*Cobble*`, `*Paving*` as high-confidence road
- [ ] Classifier excludes grass, forest floor, rock textures
- [ ] DBC chain resolves `effectId` → `GroundEffectTexture` → doodadId array
- [ ] Binary mask for Goldshire area shows recognizable road shapes at threshold=64
- [ ] No road pixels appear on layer 0 (base texture)

---

## Phase 3: Road Visualization + Multi-Tile Stitching

### Goal

Render road masks as images for visual verification. Support multi-tile stitching for continuous road networks across ADT boundaries.

### 3.1: Single-Tile Visualization

```python
# visualizer.py — key signature
def render_tile_mask(mask: np.ndarray, adt_data: AdtData,
                     confidence_map: np.ndarray = None) -> Image:
    """Render 1024x1024 road mask as a color-coded image.

    Optional confidence_map (same shape) colors pixels by classification confidence:
    - Green: high confidence (>= 0.9)
    - Yellow: medium confidence (0.5-0.9)
    - Red: low confidence (< 0.5)
    """
```

Output:
- `output/{map}_{tileX}_{tileY}_roads.png` — binary mask (white on black)
- `output/{map}_{tileX}_{tileY}_confidence.png` — confidence color-coded
- `output/{map}_{tileX}_{tileY}_alpha_raw.png` — raw alpha values (grayscale)

### 3.2: Multi-Tile Stitcher

For road network continuity across tile boundaries:

```python
# stitcher.py — key signature
def stitch_tiles(tile_masks: dict[tuple[int,int], np.ndarray],
                 tile_positions: dict[tuple[int,int], tuple[float,float]]
                 ) -> tuple[np.ndarray, float, float]:
    """Stitch multiple 1024x1024 tile masks into a single large raster.

    Returns: (stitched_raster, origin_world_x, origin_world_y)
    """
```

**Memory management**: A full continent (~40x40 tiles) at native resolution would be ~40960x40960 pixels = ~1.6 GB as bool array. Process in rectangular regions (e.g., 4x4 tiles at a time) or use 32x32 downsampled resolution for continent-scale overview.

### 3.3: Phase 3 Verification

- [ ] Single-tile image clearly shows road shapes matching in-game roads
- [ ] Confidence color coding distinguishes high/medium/low confidence regions
- [ ] Multi-tile stitching: roads are continuous across tile boundaries (no gaps or offsets)
- [ ] Output images are correctly oriented (north up, east right)

---

## Phase 4: Road Graph Extraction

### Goal

Transform binary road masks into a clean road network graph with world coordinates. Apply morphological cleanup, skeletonization, spur pruning, and graph simplification.

### 4.1: Morphological Cleanup

Before skeletonization, clean the binary mask to remove noise and fill small gaps:

```python
from skimage.morphology import binary_opening, binary_closing, disk, remove_small_objects

def cleanup_mask(mask: np.ndarray) -> np.ndarray:
    kernel = disk(1)
    cleaned = binary_closing(mask, kernel)                   # close small gaps
    cleaned = binary_opening(cleaned, kernel)                 # remove isolated noise pixels
    cleaned = remove_small_objects(cleaned, min_size=20)      # drop tiny disconnected regions
    return cleaned
```

### 4.2: Skeletonization

Use Zhang-Suen skeletonization to extract one-pixel-wide road centerlines:

```python
from skimage.morphology import skeletonize

skeleton = skeletonize(cleaned_mask)  # Zhang-Suen algorithm
```

Use `skeletonize()` (Zhang-Suen) over `medial_axis()` — scikit-image docs state it produces fewer spurious branches, ideal for elongated linear features like roads. **Must operate at native 64x64 resolution** (1024x1024 per tile). Downsampling before skeletonization collapses narrow roads and destroys intersection topology.

### 4.3: Spur Pruning

Skeletonization produces short false branches ("spurs") on irregular road edges. Remove them iteratively:

```python
from scipy.signal import convolve2d

def prune_spurs(skeleton: np.ndarray, iterations: int = 3) -> np.ndarray:
    """Remove short dead-end branches from skeleton.

    Each iteration removes one pixel from all endpoints (pixels with exactly 1 neighbor).
    3 iterations removes spurs up to 3 pixels long (~1.5 yards).
    """
    pruned = skeleton.copy()
    kern = np.ones((3, 3), dtype=int)
    for _ in range(iterations):
        neighbors = convolve2d(pruned.astype(int), kern, mode='same') - pruned.astype(int)
        endpoints = pruned & (neighbors == 1)
        pruned = pruned & ~endpoints
    return pruned
```

Tune `iterations` based on results. 3 iterations removes spurs up to ~1.5 yards. For wider roads (more edge noise), use 5-8 iterations.

### 4.4: Skeleton to Graph (sknw)

Convert the pixel skeleton to a NetworkX graph using the `sknw` library:

```python
import sknw

graph = sknw.build_sknw(skeleton.astype(np.uint16), multi=False)
# graph.nodes[i]['o'] = centroid (row, col) of node cluster
# graph[s][e]['pts'] = Nx2 array of ordered pixel coordinates along edge
# graph[s][e]['weight'] = edge length in pixels
```

Nodes are junction/endpoint pixel clusters. Edges carry ordered pixel coordinate arrays and length weights.

### 4.5: World Coordinate Assignment

Convert all pixel coordinates to WoW world coordinates:

```python
def assign_world_coords(graph: nx.Graph, tile_origin_x: float, tile_origin_y: float):
    """Assign world_x, world_y to all nodes and edge points."""
    for node_id in graph.nodes():
        r, c = graph.nodes[node_id]['o']
        graph.nodes[node_id]['world_x'] = tile_origin_x - r * PIXEL_SIZE
        graph.nodes[node_id]['world_y'] = tile_origin_y - c * PIXEL_SIZE

    for s, e in graph.edges():
        pts = graph[s][e]['pts']
        world_pts = []
        for r, c in pts:
            wx = tile_origin_x - r * PIXEL_SIZE
            wy = tile_origin_y - c * PIXEL_SIZE
            world_pts.append((wx, wy))
        graph[s][e]['world_pts'] = world_pts
```

Where `tile_origin_x` / `tile_origin_y` are the NW corner world coords of the stitched raster (for single tile: the NW-most chunk's position).

### 4.6: Edge Simplification (Douglas-Peucker)

Reduce point count on long straight edges:

```python
from rdp import rdp

for s, e in graph.edges():
    pts = graph[s][e]['world_pts']
    if len(pts) > 2:
        simplified = rdp(pts, epsilon=2.0)  # 2-yard tolerance
        graph[s][e]['world_pts'] = simplified
```

### 4.7: Multi-Tile Graph Merge

When processing multiple tiles, merge graphs at tile boundaries:

```python
def merge_tile_graphs(graphs: list[nx.Graph], merge_distance: float = 2.0) -> nx.Graph:
    """Merge multiple tile graphs, connecting nodes that are within merge_distance yards."""
    # 1. Combine all graphs into one
    # 2. For each pair of nodes from different tiles within merge_distance:
    #    - Merge them into a single node (average position)
    #    - Reconnect edges
    # 3. Remove duplicate edges
```

### 4.8: JSON Export

Export the final graph in a format compatible with the map editor's world graph system:

```python
def export_graph_json(graph: nx.Graph, output_path: str):
    """Export road graph as JSON.

    Format:
    {
        "nodes": [
            {"id": 0, "x": 1234.5, "y": -5678.9},
            ...
        ],
        "edges": [
            {"from": 0, "to": 1, "waypoints": [[x,y], [x,y], ...]},
            ...
        ],
        "metadata": {
            "map": "Azeroth",
            "tiles": [[31,48], [31,49], ...],
            "threshold": 64,
            "generated": "2026-02-25T12:00:00"
        }
    }
    """
```

### 4.9: Phase 4 Verification

- [ ] Skeleton is one-pixel-wide, connected where roads connect
- [ ] Spur pruning removes false branches without breaking main road connectivity
- [ ] Graph nodes correctly placed at intersections and dead ends
- [ ] World coordinates match expected in-game positions (verify against known Goldshire road coords)
- [ ] Douglas-Peucker simplification: straight roads have 2 points, curves retain shape
- [ ] Multi-tile merge: roads continue seamlessly across tile boundaries
- [ ] JSON export loads correctly in the map editor

---

## Dependencies

### Python Libraries

| Purpose | Library | Version | Install |
|---------|---------|---------|---------|
| MPQ archives | python-mpq | >=1.0 | `uv add mpq` |
| Image processing | Pillow | >=10.0 | `uv add Pillow` |
| Array operations | numpy | >=1.24 | `uv add numpy` |
| Morphological ops | scikit-image | >=0.21 | `uv add scikit-image` |
| Convolutions | scipy | >=1.11 | `uv add scipy` |
| Skeleton to graph | sknw | >=0.14 | `uv add sknw` |
| Graph operations | networkx | >=3.1 | `uv add networkx` |
| Line simplification | rdp | >=0.8 | `uv add rdp` |
| Visualization | matplotlib | >=3.7 | `uv add matplotlib` |

### External Dependencies

- **StormLib** shared library (`.dll` on Windows, `.so` on Linux) — required by `python-mpq`. Build from source or obtain prebuilt binary
- **WoW 3.3.5a client** — Data directory containing MPQ archives
- **GroundEffectTexture.dbc** — Extract from MPQ (`DBFilesClient\GroundEffectTexture.dbc`) for DBC classification chain
- **TerrainType.dbc** — Extract from MPQ (`DBFilesClient\TerrainType.dbc`)

---

## Critical Implementation Notes

### 1. Errors in Research Document 3

The document `Построение дорожной сети...на Python.md` (doc 3) contains several significant errors:

**MCLY flags are WRONG**: Doc 3 claims `0x100` = compressed and `0x200` = big alpha. The **correct** mapping (from wowdev.wiki and doc 1) is:
- `0x100` = `use_alpha_map` (set on layers 1-3)
- `0x200` = `alpha_map_compressed` (RLE compression)

The big alpha flag is **not in MCLY at all** — it's in the WDT's MPHD chunk (`0x0004`).

**MCNK position is NOT the SE corner**: Doc 3 assumes position is the "SE corner." The **correct** interpretation (from wowdev.wiki and doc 1) is that `MCNK.position[3]` at offset 0x68 is the **NW corner** of the chunk.

**Coordinate formulas are backwards**: Doc 3's `world_x = base + (63-u) * pixel_size` formula is incorrect. Since pixel (0,0) = NW corner and NW has max X/Y values, the correct formula is `world_x = base_x - row * PIXEL_SIZE` (subtracting, not adding).

**MCAL decompression is NOT unknown**: Doc 3 claims the compression algorithm is unknown and requires reverse engineering. In fact, it is a well-documented RLE algorithm (see doc 1, §2 and wowdev.wiki ADT/v18 page).

**Pillow DOES support BLP natively**: Doc 3 claims Pillow cannot decode BLP. Since Pillow 3.x, BLP2 is natively supported — `Image.open("texture.blp")` works directly.

**Always cross-reference doc 3 claims against doc 1 and wowdev.wiki (ADT/v18 page).**

### 2. Alpha Format Determination — Parse WDT First

The single most common pitfall is parsing alpha data with the wrong format. **Always parse the WDT file first** to read MPHD flags before processing any ADTs. For Eastern Kingdoms (Elwynn Forest, etc.), expect 4-bit packed alpha (2048 bytes per layer). For Northrend, expect 8-bit raw (4096 bytes).

### 3. ADT Filename Convention

ADT filename: `{MapName}_{TileX}_{TileY}.adt`. The mapping between tile indices and world axes can be confusing — always validate against known landmark coordinates (e.g., Goldshire Inn ≈ wowX=-9460, wowY=62).

### 4. Threshold Tuning

Start with threshold=64 (25%). If roads appear disconnected, lower to 48. If too much non-road terrain bleeds in, raise to 96. Adaptive per-tile thresholding (Otsu's method on nonzero alpha pixels) can work but may be inconsistent across zones.

### 5. Memory Management for Large Areas

A full continent at native resolution (64px/chunk) is ~40960x40960 = ~1.6 GB as bool array. Strategies:
- Process in rectangular tile groups (4x4 or 8x8 tiles at a time)
- Use 32x32 per chunk (2x downsample) for continent-scale overview — roads remain 3-8 pixels wide
- Never go below 16x16 per chunk — risk of collapsing narrow roads

### 6. No Existing Road Extraction Tools

This is novel work. No open-source project extracts road networks from WoW terrain data. Noggit treats all textures generically. TrinityCore's map extractor only extracts height/liquid/area data — no texture or alpha data. The pipeline must be built from scratch.

---

## Implementation Order

```
Phase 1: Project Setup + WDT/ADT Parser
    ├── 1.1: uv project scaffold + dependencies
    ├── 1.2: WDT parser (MPHD flags)
    ├── 1.3: ADT parser (MCNK, MTEX, MCLY)
    ├── 1.5: MCAL decoder (all 3 formats)
    ├── 1.6: Coordinate utilities
    └── 1.7: Verification (decode alpha for Elwynn tile)
         ↓
Phase 2: Road Texture Classification
    ├── 2.1: Filename pattern classifier
    ├── 2.1: DBC chain parser (GroundEffectTexture.dbc)
    ├── 2.2: Binary road mask builder
    └── 2.4: Verification (Goldshire road mask)
         ↓
Phase 3: Road Visualization + Multi-Tile Stitching
    ├── 3.1: Single-tile mask renderer
    ├── 3.2: Multi-tile stitcher
    └── 3.3: Verification (visual comparison with in-game)
         ↓
Phase 4: Road Graph Extraction
    ├── 4.1: Morphological cleanup
    ├── 4.2: Zhang-Suen skeletonization
    ├── 4.3: Spur pruning
    ├── 4.4: sknw graph construction
    ├── 4.6: Douglas-Peucker simplification
    ├── 4.7: Multi-tile graph merge
    ├── 4.8: JSON export
    └── 4.9: Verification (load in map editor)
```

**Phase 1 alone** validates the entire ADT parsing stack. Each subsequent phase builds on it incrementally. The pipeline can produce useful results (visual road masks) by end of Phase 3, before graph extraction is complete.
