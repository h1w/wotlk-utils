# Extracting road textures from WoW 3.3.5a ADT terrain files

WoW roads are not discrete objects — they are texture layers alpha-blended onto terrain chunks, identifiable through a combination of **filename pattern matching** (keywords like `Road`, `Dirt`, `Cobblestone` in MTEX paths) and the **DBC surface-type chain** (`MCLY.effectId → GroundEffectTexture.m_sound → TerrainType.m_TerrainDesc`). No MCLY flag marks a layer as "road"; classification is entirely a data-interpretation problem. The MCAL alpha map format varies by map: old-world continents default to **2048-byte 4-bit packed** while Northrend uses **4096-byte 8-bit**, determined by the WDT's `adt_has_big_alpha` flag (MPHD 0x0004). After extracting road alpha pixels, the recommended pipeline is threshold → morphological cleanup → Zhang-Suen skeletonization → sknw graph construction → Douglas-Peucker simplification.

---

## 1. ADT binary format essentials

### Chunk tags and byte order

All WoW chunked files (ADT, WDT, WMO, WDL) store IFF chunk tags as **little-endian uint32** values. In a hex editor, `MCNK` appears as bytes `4B 4E 43 4D` (i.e., `KNCM`). The wowdev.wiki confirms: *"try scanning for 'RDHM' to find MHDR."* M2 files are the sole exception — their chunk names are NOT reversed. When parsing, read the 4-byte tag as a uint32 and compare against the reversed ASCII string, or read 4 chars and reverse them.

Each IFF chunk has an 8-byte header: 4 bytes tag + 4 bytes `uint32_t size` (size of data following the header, not including the 8-byte header itself).

### MHDR offsets

The MHDR chunk data is a 64-byte structure. All offset fields within MHDR (to MCIN, MTEX, MMDX, etc.) are **relative to the start of MHDR data** — that is, the byte position of the `flags` field, immediately after the MHDR IFF 8-byte header. The MHDR `flags` field itself has minimal alpha-map relevance; the critical alpha flag lives in the WDT file's MPHD chunk (see §2).

### MCIN: absolute offsets

The MCIN chunk contains **256 entries × 16 bytes each** (4096 bytes total). Each entry is:

```c
struct SMChunkInfo {
    uint32_t offset;    // ABSOLUTE from file start
    uint32_t size;      // total MCNK chunk size
    uint32_t flags;     // always 0 on disk (FLAG_LOADED set at runtime)
    uint32_t asyncId;   // runtime only
};
```

**Offsets are absolute** from the beginning of the ADT file. Entries are ordered row-major: entry 0 = chunk (0,0) at northwest, entry 15 = chunk (0,15), entry 16 = chunk (1,0), and so on through entry 255 = chunk (15,15).

### MCNK header and sub-chunk offsets

Each MCNK begins with the 8-byte IFF header, followed by a **128-byte (0x80) MCNK header**:

```c
struct SMChunkHeader {            // 128 bytes
    /*0x00*/ uint32_t flags;
    /*0x04*/ uint32_t indexX;     // column (0–15)
    /*0x08*/ uint32_t indexY;     // row (0–15)
    /*0x0C*/ uint32_t nLayers;   // 1–4
    /*0x10*/ uint32_t nDoodadRefs;
    /*0x14*/ uint32_t ofsMCVT;   // height vertices
    /*0x18*/ uint32_t ofsMCNR;   // normals
    /*0x1C*/ uint32_t ofsMCLY;   // texture layers
    /*0x20*/ uint32_t ofsMCRF;   // doodad/WMO refs
    /*0x24*/ uint32_t ofsMCAL;   // alpha maps
    /*0x28*/ uint32_t sizeAlpha; // ACTUAL MCAL data size (use this, not the IFF chunk size!)
    /*0x2C*/ uint32_t ofsMCSH;   // shadow map
    /*0x30*/ uint32_t sizeShadow;
    /*0x34*/ uint32_t areaid;    // zone/area ID
    /*0x38*/ uint32_t nMapObjRefs;
    /*0x3C*/ uint32_t holes;     // 16-bit hole bitmask
    /*0x40*/ uint8_t  lowQualityTexMap[16]; // uint2[8][8] packed
    /*0x50*/ uint32_t predTex;
    /*0x54*/ uint32_t noEffectDoodad;
    /*0x58*/ uint32_t ofsMCSE;
    /*0x5C*/ uint32_t nSndEmitters;
    /*0x60*/ uint32_t ofsMCLQ;   // old liquid
    /*0x64*/ uint32_t sizeLiquid;
    /*0x68*/ float    position[3]; // X, Y, Z world position (NW corner)
    /*0x74*/ uint32_t ofsMCCV;
    /*0x78*/ uint32_t ofsMCLV;   // Cata+
    /*0x7C*/ uint32_t unused;
};
```

**Sub-chunk offsets (ofsMCVT, ofsMCNR, ofsMCLY, ofsMCAL, etc.) are relative to the start of the MCNK IFF tag** — not relative to MCNK data, and not absolute from file start. So:

```
file_position_of_subchunk = file_position_of_MCNK_tag + offset_value
```

For example, `ofsMCVT = 0x88` means the MCVT sub-chunk starts at the MCNK tag position + 0x88, which is 8 (IFF header) + 128 (MCNK header) = 136 = 0x88 — right after the MCNK header.

**Critical note**: The MCAL sub-chunk's IFF size field is often incorrect in Blizzard's data. Always use `sizeAlpha` from the MCNK header at offset 0x28 to determine the actual MCAL data length.

---

## 2. MCAL alpha map format in detail

### Three format variants

The format of each layer's alpha data is determined by two independent flags:

| Condition | Format | Size |
|-----------|--------|------|
| No big_alpha, no compression | **4-bit packed** | 2048 bytes |
| Big_alpha set, no compression | **8-bit raw** | 4096 bytes |
| Compression flag set (any map) | **RLE compressed** | Variable → decompresses to 4096 bytes |

**Determining the format** requires checking both the WDT-level flag and the per-layer flag:

```python
def get_alpha_format(wdt_mphd_flags, mcly_layer_flags):
    if mcly_layer_flags & 0x200:          # per-layer compressed flag
        return 'compressed'               # always decompresses to 4096 bytes (8-bit)
    elif wdt_mphd_flags & 0x0004:         # WDT big_alpha flag
        return 'uncompressed_8bit'        # 4096 bytes
    else:
        return 'uncompressed_4bit'        # 2048 bytes
```

The WDT's MPHD `adt_has_big_alpha` flag (bit 2, value **0x0004**) is the global switch. In WoW 3.3.5a, **Northrend (mapId 571) has this flag set** (8-bit default), while **old-world continents (Eastern Kingdoms mapId 0, Kalimdor mapId 1) do not** (4-bit default). You must parse the WDT file for each map to determine this.

**Layer 0 always has no alpha map.** It implicitly covers 100% of the chunk at full opacity. Only layers 1–3 carry alpha data. The `use_alpha_map` flag (MCLY 0x100) is set on layers 1–3 and never on layer 0. The blending formula is:

```
finalColor = tex0 × (1 - α1 - α2 - α3) + tex1 × α1 + tex2 × α2 + tex3 × α3
```

### 4-bit packed format (2048 bytes)

Used on old-world maps without `adt_has_big_alpha`. Each byte contains two 4-bit values, **low nibble first**:

```python
def decode_4bit_alpha(data):  # data = 2048 bytes
    alpha = np.zeros((64, 64), dtype=np.uint8)
    for row in range(64):
        for col in range(64):
            idx = (row * 64 + col) // 2
            if (row * 64 + col) % 2 == 0:
                val = data[idx] & 0x0F          # low nibble
            else:
                val = (data[idx] >> 4) & 0x0F   # high nibble
            alpha[row, col] = val | (val << 4)   # expand 0–15 → 0–255
    return alpha
```

### 8-bit uncompressed format (4096 bytes)

Simply read 4096 bytes as a 64×64 row-major array of uint8 values (0–255):

```python
def decode_8bit_alpha(data):  # data = 4096 bytes
    return np.frombuffer(data, dtype=np.uint8).reshape(64, 64)
```

### RLE compressed format

When MCLY flag **0x200** is set, the alpha data is RLE-compressed. It always decompresses to 4096 bytes (8-bit, 64×64). The algorithm uses two modes selected by the high bit of each control byte:

```python
def decompress_alpha(data):
    output = bytearray(4096)
    in_pos = 0
    out_pos = 0
    while out_pos < 4096:
        control = data[in_pos]; in_pos += 1
        count = control & 0x7F
        if control & 0x80:              # FILL mode: repeat one value
            value = data[in_pos]; in_pos += 1
            for _ in range(count):
                if out_pos >= 4096: break
                output[out_pos] = value; out_pos += 1
        else:                           # COPY mode: literal bytes
            for _ in range(count):
                if out_pos >= 4096: break
                output[out_pos] = data[in_pos]; in_pos += 1; out_pos += 1
    return np.frombuffer(bytes(output), dtype=np.uint8).reshape(64, 64)
```

**Important**: Some Blizzard ADTs contain buggy compressed data that would decompress to >4096 bytes. Noggit handles this by stopping at exactly 4096 bytes. Fill/copy runs should not cross 64-byte row boundaries, though the decompressor doesn't strictly enforce this — it simply writes sequentially.

### The do_not_fix_alpha_map flag

MCNK flags bit 15 (**0x8000**) controls edge pixel handling. When this flag is **not** set, the client "fixes" alpha maps by copying the second row over the first and the second column over the first, preventing visible seams between chunks. When **set**, raw alpha values are used directly. Additionally, when set with big_alpha mode, shadow positions cause alpha values to be scaled by approximately 0.7 (`178 × alpha >> 8`).

### MCLY flag reference (complete for v18)

| Bits | Mask | Name | Description |
|------|------|------|-------------|
| 0–2 | 0x007 | animation_rotation | Each increment = 45° clockwise |
| 3–5 | 0x038 | animation_speed | Higher = faster animation |
| 6 | 0x040 | animation_enabled | Enable texture animation |
| 7 | 0x080 | overbright | Glow effect (lava) |
| **8** | **0x100** | **use_alpha_map** | **Set on layers 1–3; never on layer 0** |
| **9** | **0x200** | **alpha_map_compressed** | **This layer's alpha is RLE-compressed** |
| 10 | 0x400 | use_cube_map_reflection | WotLK+: skybox reflection (ice surfaces) |

**There are no MCLY flag bits for road/surface type classification.** Surface type is encoded exclusively via the `effectId` field linking to DBC data.

---

## 3. Alpha map pixel to world coordinate mapping

### Coordinate system

WoW uses a left-handed coordinate system where **X points north** (positive = further north), **Y points west** (positive = further west), and **Z points up**. The map origin (0, 0) is at the center of the full 64×64 ADT grid. The northwest corner of the full map is at approximately **(17066.67, 17066.67)**.

Each ADT tile is **533.33 yards** (= 1600/3 yards). Each MCNK chunk is **33.33 yards** (= 100/3 yards). Each alpha pixel covers **≈0.5208 yards** (= 100/192 yards exactly, or 25/48 yards).

### Pixel orientation

**Pixel (0, 0) is the northwest corner** of the chunk. Row index increases southward (decreasing world X). Column index increases eastward (decreasing world Y). This is consistent with the vertex heightmap grid orientation.

### World coordinate formula

Given `MCNK.position` (the float[3] at header offset 0x68, representing the chunk's NW corner in world coords) and alpha pixel at `(row, col)`:

```python
CHUNK_SIZE = 100.0 / 3.0       # 33.3333... yards
PIXEL_SIZE = CHUNK_SIZE / 64.0  # 0.520833... yards

world_x = mcnk_position_x - row * PIXEL_SIZE   # X decreases going south
world_y = mcnk_position_y - col * PIXEL_SIZE   # Y decreases going east
```

Alternatively, computing from ADT tile coordinates and chunk indices:

```python
def pixel_to_world(adt_tile_x, adt_tile_y, chunk_row, chunk_col, pixel_row, pixel_col):
    TILE_SIZE = 533.33333
    CHUNK_SIZE = TILE_SIZE / 16.0
    PIXEL_SIZE = CHUNK_SIZE / 64.0
    MAP_ORIGIN = 17066.66666  # = 32 * TILE_SIZE

    tile_origin_x = MAP_ORIGIN - adt_tile_y * TILE_SIZE  # note: tile Y maps to world X
    tile_origin_y = MAP_ORIGIN - adt_tile_x * TILE_SIZE  # note: tile X maps to world Y

    world_x = tile_origin_x - (chunk_row * CHUNK_SIZE + pixel_row * PIXEL_SIZE)
    world_y = tile_origin_y - (chunk_col * CHUNK_SIZE + pixel_col * PIXEL_SIZE)
    return world_x, world_y
```

**Be careful with ADT tile coordinate conventions.** The ADT filename `World\Maps\Azeroth\Azeroth_32_48.adt` uses the pattern `Map_X_Y.adt` where X and Y are tile grid indices. The mapping between tile indices and world axes can be confusing — always validate against known landmark coordinates.

### Adjacent chunk behavior

Alpha maps between neighboring MCNK chunks **abut directly without overlap**. There is no shared border pixel. The `do_not_fix_alpha_map` flag affects how the client smooths the visual seam; for data extraction purposes, you can stitch chunks directly. The client uses bicubic interpolation during rendering to smooth transitions.

---

## 4. Road texture identification

### No flag-based shortcut exists

There is no MCLY flag, MCNK flag, or ADT-level marker that labels a texture layer as "road." Road identification requires examining what the texture actually is. Two complementary approaches exist.

### Approach 1: filename pattern matching (primary)

Terrain textures are stored in MPQ archives under paths like `Tileset\<ZoneName>\<ZonePrefix><SurfaceType><Number>.blp`. The MTEX chunk in each ADT lists the full paths of all textures used by that tile. Road textures follow predictable naming:

```python
ROAD_PATTERNS = [
    'road', 'dirtroad', 'cobble', 'cobblestone',
    'flagstone', 'paving', 'pavement'
]
MAYBE_ROAD_PATTERNS = ['dirt', 'path', 'gravel']  # ambiguous — includes non-road dirt
EXCLUDE_PATTERNS = ['grass', 'forest', 'leaves', 'rock', 'cliff',
                    'snow', 'sand', 'moss', 'water', 'lava', 'root']

def classify_texture(path):
    name = path.lower()
    if any(p in name for p in ROAD_PATTERNS):
        return 'road_high_confidence'
    if any(p in name for p in MAYBE_ROAD_PATTERNS):
        if not any(e in name for e in EXCLUDE_PATTERNS):
            return 'road_medium_confidence'
    return 'not_road'
```

Road textures are **zone-specific** in visual style but follow consistent naming conventions. Examples of expected MTEX entries for Elwynn Forest ADTs:

- `Tileset\Elwynn\ElwynnDirt01.blp` — dirt roads (medium confidence)
- `Tileset\Elwynn\ElwynnDirtRoad01.blp` — explicit road texture (high confidence)
- `Tileset\Elwynn\ElwynnGrass01.blp` — grass base (not road)
- `Tileset\Elwynn\ElwynnForestFloor01.blp` — forest floor (not road)
- `Tileset\Elwynn\ElwynnRock01.blp` — rocky ground (not road)
- `Tileset\Generic\GenericRoad01.blp` — shared road texture (high confidence)

Different zones use distinct textures matching their palette: Dun Morogh has grey/blue-tinted stone roads, Westfall has dry dusty paths, Barrens has red-orange dirt.

### Approach 2: DBC surface-type chain (supplementary)

Each MCLY layer has an `effectId` field (int16 at offset 0x0C) that links into the DBC chain:

```
MCLY.effectId → GroundEffectTexture.dbc (m_ID) → m_sound field
    → TerrainType.dbc (m_TerrainID) → m_TerrainDesc string
```

The `GroundEffectTexture.dbc` structure:

```c
struct GroundEffectTextureRec {
    uint32_t m_ID;              // matched by MCLY.effectId
    uint32_t m_doodadId[4];     // detail doodad types (→ GroundEffectDoodad.dbc)
    uint32_t m_doodadWeight[4]; // spawn probability weights
    uint32_t m_density;         // detail doodad density
    uint32_t m_sound;           // → TerrainType.dbc m_TerrainID
};
```

The `TerrainType.dbc` defines surface categories:

| TerrainID | m_TerrainDesc | Road relevance |
|-----------|---------------|----------------|
| 0 | Dirt | Dirt roads + natural dirt terrain |
| 1 | Metallic | Not road |
| 2 | Stone | Cobblestone roads + natural rock |
| 3 | Snow | Not road |
| 4 | Wood | Not road (planks, docks) |
| 5 | Grass | Not road |
| 6 | Leaves | Not road |
| 7 | Sand | Not road |

**There is no explicit "Road" terrain type.** Roads are classified as "Dirt" (ID 0) or "Stone" (ID 2). This means the DBC chain alone cannot distinguish a dirt road from natural dirt terrain. However, crossing it with filename matching yields high-confidence classification: a texture named `*Road*` with TerrainType "Dirt" or "Stone" is definitively a road.

### Recommended combined strategy

Use filename matching as the primary classifier (high precision for `*Road*`, `*Cobble*`, `*Paving*` patterns), DBC chain as a secondary filter (catches non-standard texture names, confirms surface type), and `GroundEffectTexture.m_density` as a tertiary signal (road surfaces typically have zero or very low doodad density compared to grass). Also note that road textures almost always appear as **layers 1–3** (overlay layers), not layer 0 (base ground).

### No existing road extraction tools

No open-source project specifically extracts road networks from WoW terrain data. Noggit treats all textures generically. TrinityCore's map extractor only extracts height, liquid, and area data — **no texture or alpha data at all**. The mmaps generator creates navmeshes from collision geometry with no road surface differentiation. This is a novel task.

---

## 5. From alpha pixels to a road network graph

### Thresholding the alpha map

WoW alpha maps use **gradual blending** — road edges have smooth alpha gradients, not hard edges. Choosing the right threshold determines road width and connectivity:

| Threshold | Behavior |
|-----------|----------|
| 30–50 (12–20%) | Very inclusive; catches faint edges, may include spurious bleed |
| **64 (25%)** | **Recommended default** — captures the visually apparent road area while maintaining connectivity |
| 128 (50%) | Only where road texture is dominant; roads appear narrow, connectivity may break |
| 192 (75%) | Too strict; roads become disconnected fragments |

**Start with 64** (25%). Because all layer alphas sum to 1.0, road textures at transitions and chunk boundaries often don't reach 50%. A lower threshold ensures connectivity, and morphological cleanup + skeletonization will find the correct centerline regardless of road width. For adaptive thresholding, Otsu's method on nonzero alpha pixels can work per-tile.

### Resolution considerations

At native 64×64 per chunk, each pixel is **≈0.52 yards**. A typical WoW road is 3–8 yards wide, rendering as **6–15 pixels**. A full ADT tile at native resolution is 1024×1024 pixels. A full continent (~40×40 ADTs) reaches ~40,960×40,960 pixels — large but feasible with tiled processing.

**32×32 per chunk** (2× downsample) is an excellent compromise: roads remain 3–8 pixels wide, connectivity is preserved, and memory drops by 4×. Going below 16×16 risks collapsing narrow roads to single pixels and losing intersection topology. **Do not go below 16×16.**

### Complete pipeline with code

```python
import numpy as np
from scipy.ndimage import label, generate_binary_structure
from skimage.morphology import skeletonize, binary_opening, binary_closing
from skimage.morphology import disk, remove_small_objects
import sknw
import networkx as nx

# === Step 1: Extract alpha maps, threshold to binary ===
THRESHOLD = 64
road_keywords = ['road', 'dirt', 'cobble', 'path', 'paving', 'flagstone', 'gravel']
tile_raster = np.zeros((1024, 1024), dtype=bool)

for chunk_row in range(16):
    for chunk_col in range(16):
        chunk = adt.chunks[chunk_row * 16 + chunk_col]
        for layer_idx in range(1, chunk.nLayers):  # skip layer 0
            tex_name = mtex_list[chunk.layers[layer_idx].textureId].lower()
            if any(kw in tex_name for kw in road_keywords):
                alpha = decode_alpha(chunk, layer_idx)  # 64×64 uint8
                r0 = chunk_row * 64
                c0 = chunk_col * 64
                tile_raster[r0:r0+64, c0:c0+64] |= (alpha > THRESHOLD)

# === Step 2: Morphological cleanup ===
kernel = disk(1)
cleaned = binary_closing(tile_raster, kernel)     # close small gaps
cleaned = binary_opening(cleaned, kernel)          # remove isolated noise
cleaned = remove_small_objects(cleaned, min_size=20)  # drop tiny regions

# === Step 3: Skeletonize (Zhang-Suen) ===
skeleton = skeletonize(cleaned)

# === Step 4: Prune short spurs ===
def prune_skeleton(skel, iterations=3):
    from scipy.signal import convolve2d
    pruned = skel.copy()
    kern = np.ones((3, 3))
    for _ in range(iterations):
        neighbors = convolve2d(pruned.astype(int), kern, mode='same') - pruned.astype(int)
        endpoints = pruned & (neighbors == 1)
        pruned = pruned & ~endpoints
    return pruned

skeleton = prune_skeleton(skeleton, iterations=3)

# === Step 5: Convert skeleton to NetworkX graph ===
graph = sknw.build_sknw(skeleton.astype(np.uint16), multi=False)

# === Step 6: Assign world coordinates ===
PIXEL_SIZE = 100.0 / 3.0 / 64.0  # ≈0.5208 yards
for node_id in graph.nodes():
    r, c = graph.nodes[node_id]['o']  # centroid (row, col)
    graph.nodes[node_id]['world_x'] = mcnk_origin_x - r * PIXEL_SIZE
    graph.nodes[node_id]['world_y'] = mcnk_origin_y - c * PIXEL_SIZE

# === Step 7: Simplify edge geometry (Douglas-Peucker) ===
from rdp import rdp
for (s, e) in graph.edges():
    pts = graph[s][e]['pts']
    if len(pts) > 2:
        graph[s][e]['pts'] = np.array(rdp(pts.tolist(), epsilon=2.0))
```

**Use `skeletonize()` (Zhang-Suen)** over `medial_axis()` — scikit-image's docs explicitly state it produces fewer spurious branches, which is ideal for elongated linear features like roads. The **sknw** library (`pip install sknw`) converts a skeleton image directly to a NetworkX graph where nodes are junction/endpoint pixel clusters and edges carry ordered pixel coordinate arrays and length weights. For more sophisticated branch analysis (filtering by branch type, length statistics), the **skan** library provides a Pandas DataFrame output with branch classification.

---

## 6. Python libraries for the full pipeline

### MPQ archive reading

The best option is **python-mpq** by jleclanche (`pip install mpq`), which provides StormLib-based Python bindings:

```python
import mpq
archive = mpq.MPQFile("common.MPQ")
if "World\\Maps\\Azeroth\\Azeroth_32_48.adt" in archive:
    data = archive.open("World\\Maps\\Azeroth\\Azeroth_32_48.adt").read()
```

An alternative is **pyStormLib** (ctypes/cffi wrapper around a pre-compiled StormLib shared library), used in production at mmo-champion. **Avoid mpyq** — it's pure Python but only handles DEFLATE/bzip2 compression, insufficient for WoW's MPQ archives which use additional compression methods.

### BLP texture decoding

**Pillow natively supports BLP2** since version 3.x — no plugins needed:

```python
from PIL import Image
img = Image.open("texture.blp")  # auto-detected format
img.save("texture.png")          # convert to any format
```

Pillow handles all BLP2 compression modes including DXT1, DXT3, DXT5, and uncompressed palettized formats. For this project, you likely don't need to decode the actual road textures visually — the filename and DBC metadata are sufficient for classification. BLP decoding would only be needed if you want to do visual/color-based texture classification.

### ADT parsing

**No fully functional Python ADT parser exists for 3.3.5a.** The `pywowlib` project (github.com/wowdev/pywowlib) targets 3.3.5a but its ADT support is self-described as "likely broken." You will need to write your own parser using `struct` module, which is straightforward given the well-documented format. Notable non-Python parsers that serve as reference implementations include **libwarcraft** (C#, "near-full read support through WotLK"), **warcraft-rs** (Rust, comprehensive ADT types for all versions including 3.3.5a), and **blizzardry** (JavaScript/Node.js, v18 ADT support).

### Complete dependency list

| Purpose | Library | Install |
|---------|---------|---------|
| MPQ archives | mpq (python-mpq) | `pip install mpq` |
| BLP textures | Pillow | `pip install Pillow` |
| ADT parsing | struct (stdlib) + numpy | `pip install numpy` |
| DBC parsing | struct (stdlib) | Built-in |
| Morphological ops | scikit-image, scipy | `pip install scikit-image scipy` |
| Skeleton → graph | sknw | `pip install sknw` |
| Graph operations | networkx | `pip install networkx` |
| Line simplification | rdp | `pip install rdp` |
| Visualization | matplotlib | `pip install matplotlib` |

---

## Conclusion

Road extraction from WoW 3.3.5a ADTs is a tractable but multi-step problem that requires careful attention to format details. The key insight is that **no single flag or database field marks roads** — you must combine texture filename heuristics with the DBC surface-type chain and alpha map thresholding. The most common pitfall is the alpha format determination: always read the WDT's MPHD flags first to know whether you're dealing with 4-bit or 8-bit alpha, then check per-layer compression flags. For Eastern Kingdoms (Elwynn Forest and surrounding zones), expect **2048-byte 4-bit packed alpha** as the default — this is easily overlooked since most documentation focuses on the cleaner 8-bit Northrend format.

The absence of any existing road extraction tool means this is novel work, but the pipeline is well-supported by mature Python libraries. The critical design choices are: threshold at **alpha > 64** to preserve road connectivity, use **Zhang-Suen skeletonization** for clean centerlines, and prefer **sknw** for straightforward graph construction. Processing at native 64×64 resolution is recommended for accuracy, with 32×32 as an acceptable fallback for memory-constrained scenarios.