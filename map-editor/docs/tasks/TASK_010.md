# TASK-010: Terrain Texture Overlay (Pre-Baked Composite from ADT)

**Status**: DONE
**Created**: 2026-02-26

---

## Overview

Add **game texture rendering** to the map editor's 3D terrain. Currently terrain renders as procedural color (solid grey / height gradient / slope shading). This task overlays the actual WoW ground textures — grass, dirt, roads, stone — on the terrain mesh by reading ADT texture layers from MPQ archives, compositing them on the CPU, and rendering the result as a textured terrain in DX11.

**Approach**: Pre-baked composite (Option B). For each MCNK chunk (16x16 per tile = 256 chunks), all texture layers are blended on the CPU into a single BGRA image, then uploaded to the GPU as a `Texture2D`. The terrain shader samples this texture using per-vertex UV coordinates.

**Goal**: Toggle-able terrain textures in 3D mode (checkbox "Terrain Textures" in Layers panel), providing a realistic ground visualization alongside the existing procedural color modes.

**Location**: `map-editor/` in the `wotlk-utils` repository.

**Data Sources**:
- ADT files (`World\Maps\{MapName}\{MapName}_{A}_{B}.adt`) from MPQ archives — contain MTEX (texture paths), MCLY (layer descriptors), MCAL (alpha maps)
- BLP texture files (`Tileset\*.blp`) from MPQ archives — the actual ground textures
- WDT files (`World\Maps\{MapName}\{MapName}.wdt`) from MPQ — MPHD flags determine alpha format (4-bit vs 8-bit)

**Existing Infrastructure** (already implemented):
- `MpqArchiveSet` — reads files from WoW MPQ archives (`src/mpq/mpq_archive.h`)
- `DecodeBlp()` — decodes BLP textures to BGRA (`src/mpq/blp_decoder.h`)
- `TerrainRenderer` — async worker thread + GPU tile cache (`src/render/terrain_renderer.h`)
- `TerrainPipeline` — DX11 shaders + render states (`src/render/terrain_pipeline.h`)
- `TerrainMesh` / `GenerateTerrainMesh()` — mesh generation from heightmaps (`src/data/terrain_mesh.h`)
- `TerrainLoader` — loads TC `.map` files (`src/data/terrain_loader.h`)
- `LayerVisibility` — UI toggle system (`src/render/graph_renderer.h`)
- `AppSettings` — JSON persistence (`src/data/app_settings.h`)
- Python ADT parser — reference implementation for MCNK/MCLY/MCAL parsing (`docs/scripts/road_extraction/src/adt_parser.py`, `mcal_decoder.py`)

---

## Phase Status Summary

| Phase | Description | Status |
|-------|-------------|--------|
| Phase 1 | C++ ADT texture parser (MTEX, MCLY, MCAL) | DONE |
| Phase 2 | CPU texture compositor (alpha-blend layers into composite) | DONE |
| Phase 3 | UV coordinates + textured terrain pipeline | DONE |
| Phase 4 | Texture renderer integration (async loading, GPU cache) | DONE |
| Phase 5 | UI toggle, settings persistence, LOD + memory optimization | DONE |

---

## Architecture Overview

### Data Flow

```
WDT file (MPQ)           --> WdtParser --> mphd_flags (alpha format selector)
                                |
ADT file (MPQ)            --> AdtTextureParser --> MTEX paths + MCLY layers + MCAL raw data
                                |
BLP textures (MPQ)        --> DecodeBlp() --> BGRA pixel arrays (cached)
                                |
                                v
                   TerrainTextureCompositor (CPU, worker thread)
                   For each MCNK chunk (256 per tile):
                     1. Decode alpha maps (4-bit/8-bit/RLE)
                     2. Tile each BLP texture across chunk area
                     3. Alpha-blend layers 0..3 --> single BGRA image
                                |
                                v
                   CompositeTexture (per-tile, 256 chunks --> atlas or array)
                                |
                                v
                   GPU upload: Texture2D + SRV  (main thread, throttled)
                                |
                                v
                   TerrainTexturePipeline (new DX11 shader)
                   VS: transform pos, pass UV
                   PS: sample composite texture, apply lighting
```

### Render Pipeline (modified)

```
1. Clear backbuffer + depth                                          [existing]
2. Render ground plane (minimap at Z=0)                              [existing]
3. Render terrain heightmap:
   a. If textures enabled: TerrainTexturePipeline (textured + lit)   [NEW]
   b. Else: TerrainPipeline (procedural color)                       [existing]
4. Render buildings (opaque)                                         [existing]
5. Render navmesh (semi-transparent overlay)                         [existing]
6. Render overlays (graph, routes, paths)                            [existing]
7. ImGui panels                                                      [existing]
```

### New Files

```
src/data/adt_texture_parser.h       -- ADT chunk parsing (MTEX, MCLY, MCAL)
src/data/adt_texture_parser.cpp
src/data/mcal_decoder.h             -- Alpha map decode (4-bit, 8-bit, RLE)
src/data/mcal_decoder.cpp
src/data/terrain_texture_compositor.h   -- CPU compositing (layers -> composite BGRA)
src/data/terrain_texture_compositor.cpp
src/render/terrain_texture_pipeline.h   -- New DX11 shader pipeline (textured terrain)
src/render/terrain_texture_pipeline.cpp
```

### Modified Files

```
src/data/terrain_mesh.h             -- Add UV coordinates to TerrainVertex
src/data/terrain_mesh.cpp           -- Generate UV coords in GenerateTerrainMesh()
src/render/terrain_pipeline.h       -- Add TEXCOORD to vertex layout (or create separate pipeline)
src/render/terrain_pipeline.cpp     -- Update shaders to optionally sample texture
src/render/terrain_renderer.h       -- Add texture loading + composite cache
src/render/terrain_renderer.cpp     -- Worker thread: ADT parse + composite + GPU upload
src/render/graph_renderer.h         -- Add showTerrainTextures to LayerVisibility
src/ui/layer_panel.cpp              -- Add "Terrain Textures" checkbox
src/data/app_settings.h             -- Add showTerrainTextures field
src/data/app_settings.cpp           -- Persist showTerrainTextures
src/app.h                           -- Pass MpqArchiveSet to TerrainRenderer
src/app.cpp                         -- Wire texture toggle to render path
```

---

## Phase 1: C++ ADT Texture Parser

### Goal

Port the Python ADT parser (`adt_parser.py` + `mcal_decoder.py`) to C++ for reading texture layers and alpha maps from ADT files via MPQ.

### 1.1: WDT MPHD Flags Reader

The WDT file determines the alpha map format for all ADT tiles of a map.

**WDT file path**: `World\Maps\{MapName}\{MapName}.wdt` (inside MPQ)

**Parsing**:
1. Find `MPHD` chunk (reversed tag `b"DHPM"`, IFF format: 4-byte tag + 4-byte size + data)
2. Read `uint32_t flags` at offset 0 within MPHD data
3. Flag `0x004` (`adt_has_big_alpha`) → 8-bit alpha maps; absence → 4-bit packed

**Data structure**:
```cpp
struct WdtInfo {
    uint32_t mphdFlags = 0;     // MPHD flags
    bool hasBigAlpha() const { return (mphdFlags & 0x004) != 0; }
};
```

**Map name resolution**: The map name (e.g. "Azeroth", "Kalimdor") is needed to construct the WDT path. This must come from either:
- Map.dbc (mapId -> InternalName), already used by MinimapTileCache
- Or passed as parameter from the existing map selection UI

**Known values**: Azeroth MPHD flags = `0x00000000` (4-bit packed alpha).

### 1.2: ADT Texture Chunk Parser

Parse the texture-related chunks from an ADT file.

**ADT file path**: `World\Maps\{MapName}\{MapName}_{A}_{B}.adt` where `A` = tileY, `B` = tileX (WoW ADT naming convention, see MEMORY.md).

**IFF chunk navigation** (same as Python parser):
```cpp
// Tags are stored REVERSED in file (e.g. MCNK stored as 'KNCM')
// Format: [4-byte tag][4-byte size][data...]
// Returns: offset to data, data size
bool FindChunk(const uint8_t* data, size_t dataSize,
               uint32_t reversedTag, size_t startOffset,
               size_t& outDataOffset, size_t& outDataSize);
```

**Chunks to parse**:

#### MVER (version check)
- Verify version == 18 (WoW 3.3.5a)

#### MTEX (texture paths)
- Null-separated list of texture file paths
- Example: `"Tileset\\Elwynn\\ElwynnGrass01.blp\0Tileset\\Elwynn\\ElwynnDirt.blp\0..."`
- Output: `std::vector<std::string> texturePaths`

#### MCIN (chunk index)
- 256 entries, each 16 bytes: `{ uint32_t offset, uint32_t size, uint32_t flags, uint32_t asyncId }`
- `offset` is **absolute** file position of each MCNK chunk
- Provides random access to any of the 256 chunks

#### MCNK (terrain chunk, 256 per ADT)
Each MCNK contains sub-chunks MCLY and MCAL.

**MCNK header** (128 bytes from tag start + 8):
```
Offset  Size  Field
0x04    4     indexX (column 0-15)
0x08    4     indexY (row 0-15)
0x0C    4     nLayers (1-4)
0x1C    4     ofsMCLY (offset to MCLY, relative to MCNK start)
0x24    4     ofsMCAL (offset to MCAL, relative to MCNK start)
0x28    4     sizeAlpha (MCAL data size in bytes)
0x68    12    position (float3: wowX, wowZ, wowY) -- NW corner of chunk
```

**Critical**: Position at 0x68 is `(wowX, wowZ, wowY)` — empirically verified (see MEMORY.md).

#### MCLY (layer descriptors, within MCNK)
Each entry is 16 bytes:
```cpp
struct MclyEntry {
    uint32_t textureId;       // Index into MTEX string list
    uint32_t flags;           // 0x100 = use_alpha_map, 0x200 = compressed (RLE)
    uint32_t offsetInMcal;    // Byte offset within MCAL data
    uint32_t effectId;        // GroundEffectTexture.dbc link (unused here)
};
```
- Layer 0: always base (no alpha map, `flags & 0x100 == 0`)
- Layers 1-3: have alpha maps (`flags & 0x100 != 0`)

#### MCAL (alpha map raw data, within MCNK)
Raw bytes at `mcnkStart + ofsMCAL`. Each layer's alpha starts at `mcalStart + mcly[i].offsetInMcal`.

**Output data structure**:
```cpp
struct AdtChunkTexture {
    int indexX, indexY;                   // Chunk position (0-15, 0-15)
    float posX, posZ, posY;              // NW corner world coords
    int nLayers;                         // 1-4
    struct Layer {
        uint32_t textureId;              // Index into texturePaths
        uint32_t flags;
        std::vector<uint8_t> alphaRaw;   // Raw MCAL bytes for this layer
    };
    Layer layers[4];
};

struct AdtTextureData {
    bool valid = false;
    int tileX, tileY;
    std::vector<std::string> texturePaths;  // From MTEX
    AdtChunkTexture chunks[16][16];         // 256 chunks
};
```

### 1.3: MCAL Alpha Map Decoder

Port `mcal_decoder.py` to C++. Three formats:

#### 4-bit Packed (2048 bytes -> 64x64)
```cpp
void Decode4BitAlpha(const uint8_t* src, size_t srcSize, uint8_t out[64][64]) {
    // 2048 bytes: each byte = 2 pixels (low nibble first, high nibble second)
    for (int i = 0; i < 2048; ++i) {
        uint8_t lo = src[i] & 0x0F;
        uint8_t hi = (src[i] >> 4) & 0x0F;
        int idx = i * 2;
        int row = idx / 64, col = idx % 64;
        out[row][col]     = lo | (lo << 4);   // expand 0-15 -> 0-255
        out[row][col + 1] = hi | (hi << 4);   // (but col+1 may wrap -- handle linearly)
    }
    // Linear indexing is simpler:
    // alpha[idx]   = lo | (lo << 4)
    // alpha[idx+1] = hi | (hi << 4)
}
```

#### 8-bit Uncompressed (4096 bytes -> 64x64)
```cpp
void Decode8BitAlpha(const uint8_t* src, size_t srcSize, uint8_t out[64][64]) {
    std::memcpy(out, src, 4096);
}
```

#### RLE Compressed (variable -> 4096 bytes -> 64x64)
```cpp
void DecodeRleAlpha(const uint8_t* src, size_t srcSize, uint8_t out[64][64]) {
    uint8_t* dst = &out[0][0];
    size_t inPos = 0, outPos = 0;
    while (outPos < 4096 && inPos < srcSize) {
        uint8_t control = src[inPos++];
        int count = control & 0x7F;
        if (control & 0x80) {
            // FILL: repeat next byte 'count' times
            uint8_t value = src[inPos++];
            for (int i = 0; i < count && outPos < 4096; ++i)
                dst[outPos++] = value;
        } else {
            // COPY: copy 'count' literal bytes
            for (int i = 0; i < count && outPos < 4096 && inPos < srcSize; ++i)
                dst[outPos++] = src[inPos++];
        }
    }
    // Zero-fill remainder if decompression ends early
    while (outPos < 4096)
        dst[outPos++] = 0;
}
```

**Format selection** (per layer):
```cpp
enum class AlphaFormat { Packed4Bit, Uncompressed8Bit, CompressedRle };

AlphaFormat GetAlphaFormat(uint32_t mphdFlags, uint32_t mclyFlags) {
    if (mclyFlags & 0x200)          return AlphaFormat::CompressedRle;
    if (mphdFlags & 0x004)          return AlphaFormat::Uncompressed8Bit;
    return AlphaFormat::Packed4Bit;
}
```

### 1.4: Alpha Map Size Calculation

Each layer's alpha data starts at `mcly[i].offsetInMcal` within the MCAL block. The size of each layer's alpha data is determined by:
- **4-bit packed**: exactly 2048 bytes
- **8-bit uncompressed**: exactly 4096 bytes
- **RLE compressed**: variable; bounded by `mcly[i+1].offsetInMcal - mcly[i].offsetInMcal` for non-last layers, or `sizeAlpha - mcly[i].offsetInMcal` for the last layer

### Phase 1 Deliverables

- [x] `adt_texture_parser.h/.cpp` — `AdtTextureParser::Parse(MpqArchiveSet&, mapName, tileX, tileY) -> AdtTextureData`
- [x] `mcal_decoder.h/.cpp` — `DecodeAlpha(format, src, srcSize, out[64][64])`
- [x] WDT MPHD reader (can be inside `AdtTextureParser` or separate utility)
- [x] Unit validation: parse Azeroth 31,49 (Goldshire), verify MTEX list matches Python output

---

## Phase 2: CPU Texture Compositor

### Goal

Blend up to 4 texture layers into a single composite BGRA image per MCNK chunk.

### 2.1: BLP Texture Cache

Terrain textures are shared across many chunks. Cache decoded BLP images to avoid redundant MPQ reads + decode.

```cpp
class BlpTextureCache {
public:
    // Returns pointer to cached BGRA image. Thread-safe.
    const BlpImage* Get(const std::string& blpPath, MpqArchiveSet& mpq);

private:
    std::mutex m_mutex;
    std::unordered_map<std::string, BlpImage> m_cache;
};
```

**Typical texture sizes**: 256x256 or 128x128 (BLP mip level 0).
**Memory estimate**: ~100 unique textures per map x 256x256x4 = ~25 MB. Acceptable.

### 2.2: Chunk Compositing Algorithm

For each MCNK chunk, produce a composite image of resolution `COMPOSITE_SIZE x COMPOSITE_SIZE`.

**Composite resolution options**:
- 64x64 — matches alpha map resolution exactly (cheapest, ~16 KB per chunk)
- 128x128 — 4x alpha, good quality/performance balance (~64 KB per chunk)
- 256x256 — matches typical BLP texture size (~256 KB per chunk)

**Recommended**: 64x64 as default (matches alpha map resolution). Higher resolutions show texture detail from the BLP tiling, but add significant memory. The alpha map is the bottleneck — at 64x64, each pixel covers ~0.52 yards, which is more than enough for a map editor.

**Compositing pseudocode**:
```cpp
void CompositeChunk(const AdtChunkTexture& chunk,
                    const std::vector<std::string>& texturePaths,
                    BlpTextureCache& texCache,
                    MpqArchiveSet& mpq,
                    uint32_t mphdFlags,
                    uint8_t outBgra[COMPOSITE_SIZE][COMPOSITE_SIZE][4])
{
    const int CS = COMPOSITE_SIZE;

    // Decode alpha maps for layers 1-3
    uint8_t alpha[4][64][64];
    memset(alpha[0], 255, 64*64);   // Layer 0 = base, 100% opaque

    for (int l = 1; l < chunk.nLayers; ++l) {
        if (!(chunk.layers[l].flags & 0x100)) continue;  // No alpha map
        auto fmt = GetAlphaFormat(mphdFlags, chunk.layers[l].flags);
        DecodeAlpha(fmt, chunk.layers[l].alphaRaw.data(),
                    chunk.layers[l].alphaRaw.size(), alpha[l]);
    }

    // For each output pixel
    for (int py = 0; py < CS; ++py) {
        for (int px = 0; px < CS; ++px) {
            // Alpha map coordinates (bilinear or nearest from 64x64)
            int ax = px * 64 / CS;
            int ay = py * 64 / CS;

            // Start with layer 0 (base)
            float r = 0, g = 0, b = 0;

            for (int l = 0; l < chunk.nLayers; ++l) {
                const BlpImage* tex = texCache.Get(texturePaths[chunk.layers[l].textureId], mpq);
                if (!tex) continue;

                // Tile the texture across the chunk
                // WoW tiles ground textures ~8 times per chunk (64m / 8m ≈ 8 repeats)
                float texScale = 8.0f;  // ~8 repeats per chunk (tunable)
                int tu = (int)(px * texScale * tex->width / CS) % tex->width;
                int tv = (int)(py * texScale * tex->height / CS) % tex->height;

                int texIdx = (tv * tex->width + tu) * 4;
                float tr = tex->bgra[texIdx + 2] / 255.0f;  // BGRA -> R
                float tg = tex->bgra[texIdx + 1] / 255.0f;  // BGRA -> G
                float tb = tex->bgra[texIdx + 0] / 255.0f;  // BGRA -> B

                if (l == 0) {
                    r = tr; g = tg; b = tb;
                } else {
                    float a = alpha[l][ay][ax] / 255.0f;
                    r = r * (1.0f - a) + tr * a;
                    g = g * (1.0f - a) + tg * a;
                    b = b * (1.0f - a) + tb * a;
                }
            }

            outBgra[py][px][0] = (uint8_t)(b * 255.0f);  // B
            outBgra[py][px][1] = (uint8_t)(g * 255.0f);  // G
            outBgra[py][px][2] = (uint8_t)(r * 255.0f);  // R
            outBgra[py][px][3] = 255;                      // A (opaque)
        }
    }
}
```

### 2.3: Tile Atlas Assembly

256 chunks per tile, each 64x64 pixels. Two options:

**Option A: Texture2DArray** (256 slices of 64x64)
- Simple addressing: `texArray.Sample(sampler, float3(u, v, chunkIndex))`
- Requires per-vertex chunk index or per-chunk draw call
- Clean but adds complexity to vertex format

**Option B: Single Texture Atlas** (16x16 grid of 64x64 = 1024x1024)
- One texture per tile
- UV: `atlasU = (chunkCol + localU) / 16.0`, `atlasV = (chunkRow + localV) / 16.0`
- More natural UV mapping, single draw call per tile
- **Recommended**

**Atlas layout** (1024x1024 BGRA = 4 MB per tile):
```
chunkRow 0:  [chunk 0,0] [chunk 0,1] ... [chunk 0,15]
chunkRow 1:  [chunk 1,0] [chunk 1,1] ... [chunk 1,15]
...
chunkRow 15: [chunk 15,0] ...            [chunk 15,15]
```

Each 64x64 block in the atlas corresponds to one MCNK chunk's composite texture.

### 2.4: Memory Budget

Per tile: 1024 x 1024 x 4 = **4 MB** (BGRA atlas)
150 cached tiles x 4 MB = **600 MB** (worst case, all tiles have textures)

**Optimizations**:
- BC1 compression on GPU: 1024x1024 x 0.5 bpp = **0.5 MB per tile** (8x reduction)
- Only load textures for close tiles (LOD gating): far tiles use procedural color
- Share BLP texture cache globally (~25 MB for ~100 textures)

**With BC1**: 150 tiles x 0.5 MB = **75 MB** total GPU memory. Very reasonable.

### Phase 2 Deliverables

- [x] `terrain_texture_compositor.h/.cpp` — `CompositeChunk()` + `AssembleTileAtlas()`
- [x] `BlpTextureCache` class (thread-safe, global lifetime)
- [x] Atlas output: 1024x1024 BGRA `std::vector<uint8_t>` ready for GPU upload
- [x] Validation: render atlas for Goldshire tile to PNG, visual comparison with WoW client

---

## Phase 3: UV Coordinates + Textured Terrain Pipeline

### Goal

Extend terrain vertex format with UV coordinates and create a new shader pipeline that samples the composite texture atlas.

### 3.1: UV Coordinate Generation

**New vertex format**:
```cpp
struct TerrainVertex {
    float x, y, z;     // WoW world coordinates
    float nx, ny, nz;  // Normal vector
    float u, v;         // Texture coordinate (atlas UV)
};
// 32 bytes (was 24 bytes, +33%)
```

**UV mapping strategy**: Atlas UV maps directly from world position within the tile.

The tile covers a 16x16 grid of chunks. Each chunk is 64x64 pixels in the atlas (total 1024x1024). UV coordinates:

```cpp
// In GenerateTerrainMesh(), for each vertex:
float tileOriginX = (32 - tileX) * TILE_SIZE;  // north edge (max wowX)
float tileOriginY = (32 - tileY) * TILE_SIZE;  // west edge (max wowY)

// Vertex world position relative to tile origin:
// v.x ranges from tileOriginX (north) to tileOriginX - TILE_SIZE (south)
// v.y ranges from tileOriginY (west) to tileOriginY - TILE_SIZE (east)

// UV: (0,0) = NW corner, (1,1) = SE corner
v.u = (tileOriginY - v.y) / TILE_SIZE;   // 0 at west edge, 1 at east edge
v.v = (tileOriginX - v.x) / TILE_SIZE;   // 0 at north edge, 1 at south edge
```

**Note**: UV coordinates are generated regardless of whether textures are enabled. When textures are off, the shader ignores UVs. The +8 bytes per vertex overhead is negligible.

### 3.2: GPU Vertex Format Update

```cpp
struct TerrainVertexGpu {
    float x, y, z;       // 12 bytes
    float nx, ny, nz;    // 12 bytes
    float u, v;           // 8 bytes
};                        // 32 bytes total

// Input layout:
D3D11_INPUT_ELEMENT_DESC layout[] = {
    {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT,    0,  0, D3D11_INPUT_PER_VERTEX_DATA, 0},
    {"NORMAL",   0, DXGI_FORMAT_R32G32B32_FLOAT,    0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0},
    {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,        0, 24, D3D11_INPUT_PER_VERTEX_DATA, 0},
};
```

### 3.3: Textured Terrain Pipeline

New pipeline (`TerrainTexturePipeline`) or extend existing `TerrainPipeline` with a second shader pair.

**Vertex Shader** (textured):
```hlsl
cbuffer TerrainCB : register(b0) {
    float4x4 viewProj;
    float4 lightDir;      // xyz=direction, w=ambient
    float4 baseColor;
    float4 heightParams;
};

struct VS_IN  { float3 pos : POSITION; float3 norm : NORMAL; float2 uv : TEXCOORD0; };
struct VS_OUT {
    float4 clipPos  : SV_POSITION;
    float3 normal   : TEXCOORD0;
    float3 worldPos : TEXCOORD1;
    float2 uv       : TEXCOORD2;
};

VS_OUT main(VS_IN i) {
    VS_OUT o;
    float3 pos = i.pos;
    float slope = 1.0 - abs(i.norm.z);
    pos.z += baseColor.a * (1.0 + slope * 5.0);
    o.clipPos  = mul(float4(pos, 1.0), viewProj);
    o.normal   = i.norm;
    o.worldPos = i.pos;
    o.uv       = i.uv;
    return o;
}
```

**Pixel Shader** (textured):
```hlsl
Texture2D    texAtlas : register(t0);
SamplerState samLinear : register(s0);

cbuffer TerrainCB : register(b0) {
    float4x4 viewProj;
    float4 lightDir;
    float4 baseColor;
    float4 heightParams;
};

struct PS_IN {
    float4 clipPos  : SV_POSITION;
    float3 normal   : TEXCOORD0;
    float3 worldPos : TEXCOORD1;
    float2 uv       : TEXCOORD2;
};

float4 main(PS_IN i) : SV_TARGET {
    // Normal computation (same as existing terrain shader)
    float3 N;
    if (heightParams.w > -1.5 && heightParams.w < -0.5)
        N = normalize(i.normal);
    else {
        float3 dpdx = ddx(i.worldPos);
        float3 dpdy = ddy(i.worldPos);
        N = -normalize(cross(dpdx, dpdy));
    }

    float3 L = normalize(lightDir.xyz);
    float NdotL = dot(N, L);
    if (NdotL < 0.0) { N = -N; NdotL = -NdotL; }

    float ambient = lightDir.w;
    float lighting = ambient + (1.0 - ambient) * NdotL;

    // Sample composite texture
    float3 texColor = texAtlas.Sample(samLinear, i.uv).rgb;

    return float4(texColor * lighting, 1.0);
}
```

**Sampler state**:
```cpp
D3D11_SAMPLER_DESC sd = {};
sd.Filter   = D3D11_FILTER_MIN_MAG_MIP_LINEAR;  // Bilinear filtering
sd.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;       // Clamp to tile edges
sd.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
sd.MaxAnisotropy = 1;
sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
sd.MaxLOD = D3D11_FLOAT32_MAX;
```

### 3.4: Render State

Same as existing terrain pipeline:
- Blend: OPAQUE
- Rasterizer: BACK cull, SOLID fill
- Depth: LESS test, depth write ON
- The only difference: `PSSetShaderResources(0, 1, &srv)` + `PSSetSamplers(0, 1, &sampler)`

### Phase 3 Deliverables

- [x] `TerrainVertex` updated with `float u, v`
- [x] `TerrainVertexGpu` updated with UV, input layout with TEXCOORD
- [x] `GenerateTerrainMesh()` generates correct UV coordinates
- [x] `terrain_texture_pipeline.h/.cpp` — new pipeline with textured shaders
- [x] Sampler state + SRV binding in render path
- [x] Validation: hard-coded test texture, verify UV mapping is correct

---

## Phase 4: Texture Renderer Integration

### Goal

Integrate texture loading into the existing `TerrainRenderer` async worker thread, add texture GPU cache alongside geometry cache, and wire the textured render path.

### 4.1: Extended Worker Thread

The existing `TerrainRenderer::WorkerLoop()` loads `.map` files and generates mesh. Extend to also:

1. Read the ADT file from MPQ (via `AdtTextureParser`)
2. Composite the 1024x1024 atlas (via `TerrainTextureCompositor`)
3. Return the atlas BGRA bytes alongside geometry

**Extended LoadResult**:
```cpp
struct LoadResult {
    uint32_t mapId;
    int tileX, tileY;
    int decimation;
    std::vector<TerrainVertex> vertices;   // Now includes UVs
    std::vector<uint32_t> indices;
    float minX, minY, minZ, maxX, maxY, maxZ;
    bool valid;

    // NEW: texture data
    std::vector<uint8_t> textureAtlas;     // 1024x1024x4 BGRA (or empty if no ADT)
    bool hasTexture = false;
};
```

**Worker flow** (when textures enabled):
```
1. Load .map file (heightmap) — existing
2. GenerateTerrainMesh() — existing (now with UVs)
3. Parse ADT from MPQ (AdtTextureParser)
4. Composite atlas (TerrainTextureCompositor)
5. Post result to main thread
```

**Key consideration**: MPQ access. The worker thread needs access to `MpqArchiveSet`. StormLib is thread-safe for reads when using separate file handles. The `MpqArchiveSet` should be shared (read-only access from worker).

### 4.2: GPU Texture Cache

**Extended TileGpu**:
```cpp
struct TileGpu {
    ID3D11Buffer* vb = nullptr;
    ID3D11Buffer* ib = nullptr;
    UINT indexCount = 0;
    float minX, minY, minZ, maxX, maxY, maxZ;
    int decimation = 0;

    // NEW: texture atlas
    ID3D11Texture2D*          tex = nullptr;
    ID3D11ShaderResourceView* srv = nullptr;
    bool hasTexture = false;
};
```

**GPU upload** (in `UploadToGpu`):
```cpp
if (result.hasTexture && !result.textureAtlas.empty()) {
    D3D11_TEXTURE2D_DESC td = {};
    td.Width  = 1024;
    td.Height = 1024;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;   // BGRA (matches BLP decoder output)
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_IMMUTABLE;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    D3D11_SUBRESOURCE_DATA srd = {};
    srd.pSysMem = result.textureAtlas.data();
    srd.SysMemPitch = 1024 * 4;

    device->CreateTexture2D(&td, &srd, &tile.tex);
    device->CreateShaderResourceView(tile.tex, nullptr, &tile.srv);
    tile.hasTexture = true;
}
```

### 4.3: Render Path Selection

In `TerrainRenderer::Render()`:

```cpp
void Render(const Camera3D& camera, ID3D11RenderTargetView* rtv,
            ID3D11DepthStencilView* dsv, bool useTextures)
{
    for (auto& [key, tile] : m_gpuCache) {
        if (!FrustumIntersectsAABB(planes, tile)) continue;

        if (useTextures && tile.hasTexture) {
            // Bind textured pipeline + tile SRV
            BindTexturePipeline(tile.srv);
        } else {
            // Bind procedural pipeline (existing)
            BindProceduralPipeline();
        }

        // Draw (same VB/IB for both paths)
        context->DrawIndexed(tile.indexCount, 0, 0);
    }
}
```

**Fallback**: If a tile has no texture (ADT not found in MPQ, or textures disabled), it renders with the existing procedural color pipeline. This means textured and non-textured tiles can coexist seamlessly.

### 4.4: MpqArchiveSet Threading

The `MpqArchiveSet` is owned by `App`. To pass it to the terrain worker thread:

```cpp
void TerrainRenderer::SetMpqArchive(MpqArchiveSet* mpq);  // Store pointer
```

StormLib `SFileReadFile` is thread-safe when each thread opens its own file handle (via `SFileOpenFileEx`). The `MpqArchiveSet::ReadFile()` method creates+destroys handles per call — this should be thread-safe by default. Verify with a stress test.

If thread safety is an issue, the worker can buffer the raw ADT bytes on the main thread before queuing.

### Phase 4 Deliverables

- [x] Worker thread extended with ADT parse + composite
- [x] `TileGpu` extended with `Texture2D` + `SRV`
- [x] GPU upload of texture atlas in `UploadToGpu()`
- [x] Render path selection (textured vs procedural)
- [x] `MpqArchiveSet` pointer passed to renderer
- [x] Release `tex`/`srv` in `ReleaseTileGpu()`
- [x] Validation: Goldshire area with textures visible

---

## Phase 5: UI Toggle, Settings Persistence, LOD + Memory Optimization

### Goal

Expose the texture toggle in the UI, persist it across sessions, and optimize memory/performance for production use.

### 5.1: LayerVisibility Extension

**In `graph_renderer.h`** (where `LayerVisibility` is defined):
```cpp
struct LayerVisibility {
    // ... existing fields ...
    bool showTerrainTextures = false;  // NEW: terrain texture overlay (default OFF)
};
```

**Default OFF**: Textures require MPQ access (wowDir configured) and add loading time. Users who don't need textures shouldn't pay the cost.

### 5.2: Layer Panel Checkbox

**In `layer_panel.cpp`**, inside the `if (layers.showTerrain)` block, after the "Smooth" checkbox:

```cpp
ImGui::Checkbox("Terrain", &layers.showTerrain);
if (layers.showTerrain) {
    ImGui::Indent(20.0f);
    // ... existing Terrain Color combo + Smooth checkbox ...

    ImGui::Checkbox("Textures", &layers.showTerrainTextures);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Overlay game ground textures on terrain.\n"
                          "Requires WoW client Data directory (MPQ).\n"
                          "Increases loading time and memory usage.");

    ImGui::Unindent(20.0f);
}
```

**Interaction with Terrain Color mode**: When textures are ON, the Terrain Color combo (Solid Grey / Height Gradient / Slope) is hidden or dimmed, since the texture replaces procedural coloring. When textures are OFF, the combo is shown as before.

### 5.3: Settings Persistence

**In `app_settings.h`**:
```cpp
bool showTerrainTextures = false;  // NEW
```

**In `app_settings.cpp`** (Load):
```cpp
showTerrainTextures = layers.value("terrain_textures", false);
```

**In `app_settings.cpp`** (Save):
```cpp
{"terrain_textures", showTerrainTextures},
```

### 5.4: LOD-Gated Texture Loading

Only load textures for tiles within a distance threshold. Far tiles use procedural color.

```cpp
static constexpr float kTextureLoadDistance = 3.0f;  // camera distance units

// In UpdateViewport(), when queuing LoadRequest:
request.loadTextures = (tileDist < kTextureLoadDistance);
```

This reduces worker load and memory for distant tiles (which use LOD 4 = quarter resolution anyway).

### 5.5: BC1 GPU Compression (Optional Optimization)

After compositing the 1024x1024 BGRA atlas on CPU, compress to BC1 (DXT1) before GPU upload:

- **CPU-side BC1 encoding**: Use a lightweight encoder (e.g., `stb_dxt.h` or manual block encoding)
- **GPU format**: `DXGI_FORMAT_BC1_UNORM` instead of `DXGI_FORMAT_B8G8R8A8_UNORM`
- **Memory reduction**: 4 MB -> 0.5 MB per tile (8x)
- **Quality**: Minimal visual loss for terrain textures at 64x64 block resolution

**BC1 encoding pseudocode** (512 KB output for 1024x1024):
```cpp
// 1024x1024 -> 256x256 blocks of 4x4 pixels
// Each block: 2x RGB565 endpoints + 16x 2-bit indices = 8 bytes
// Total: 256*256 * 8 = 524,288 bytes
```

This optimization is optional and can be deferred to a later iteration if the raw BGRA approach works well.

### 5.6: Texture Cache Eviction

Current terrain cache holds up to 150 tiles. With textures, each tile adds ~4 MB (raw) or ~0.5 MB (BC1). The existing LRU-like eviction in `TerrainRenderer` should release `tex`/`srv` alongside `vb`/`ib` when tiles are evicted.

Ensure `ReleaseTileGpu()` handles texture resources:
```cpp
void TerrainRenderer::ReleaseTileGpu(TileGpu& tile) {
    if (tile.vb)  { tile.vb->Release();  tile.vb = nullptr; }
    if (tile.ib)  { tile.ib->Release();  tile.ib = nullptr; }
    if (tile.srv) { tile.srv->Release(); tile.srv = nullptr; }  // NEW
    if (tile.tex) { tile.tex->Release(); tile.tex = nullptr; }  // NEW
    tile.hasTexture = false;
}
```

### 5.7: Graceful Degradation

Handle edge cases:
- **No wowDir configured**: Skip texture loading silently, use procedural color
- **ADT not found in MPQ**: Some tiles may not have ADT files (ocean tiles). Render with procedural color
- **BLP decode failure**: Use magenta (255,0,255) placeholder for missing textures
- **Worker thread timeout**: If compositing takes too long, return result without texture

### Phase 5 Deliverables

- [x] `showTerrainTextures` in `LayerVisibility` + `AppSettings`
- [x] Checkbox in Layer Panel with tooltip
- [x] Settings JSON persistence (load/save)
- [x] LOD-gated texture loading (distance threshold)
- [x] Texture resource cleanup in `ReleaseTileGpu()`
- [x] Graceful degradation (missing MPQ, missing ADT, decode errors)
- [ ] Optional: BC1 compression for GPU memory savings (deferred)

---

## Performance Estimates

### Loading Time (per tile, in worker thread)

| Step | Time Estimate | Notes |
|------|--------------|-------|
| .map file read + mesh gen | ~5 ms | Existing, unchanged |
| ADT read from MPQ | ~10-20 ms | StormLib decompression |
| MCAL decode (256 chunks) | ~1-2 ms | Simple byte operations |
| BLP decode (4-8 textures) | ~2-5 ms | DXT decompression, cached after first |
| Composite (256 chunks, 64x64) | ~5-10 ms | 256 x 4096 pixel blends |
| **Total per tile** | **~25-40 ms** | vs ~5 ms without textures |

**Impact**: Tile loading ~5-8x slower, but all in background thread. User sees tiles pop in slightly later. Acceptable for a map editor.

### Runtime FPS Impact

| Metric | Without Textures | With Textures | Delta |
|--------|-----------------|---------------|-------|
| VS work | 24 bytes/vert | 32 bytes/vert | +33% bandwidth |
| PS work | 0 tex samples | 1 tex sample | +1 sample/pixel |
| GPU memory | ~1.5 MB/tile | ~5.5 MB/tile (raw) | +4 MB/tile |
| Draw calls | 1 per tile | 1 per tile | same |

**Estimated FPS impact**: 5-10% with raw BGRA, <5% with BC1. On a GTX 1060+, terrain rendering is not the bottleneck (buildings and navmesh are heavier).

### Memory Footprint

| Component | Memory |
|-----------|--------|
| BLP texture cache | ~25 MB (100 textures x 256KB) |
| Atlas composites (150 tiles, raw BGRA) | ~600 MB |
| Atlas composites (150 tiles, BC1) | ~75 MB |
| Vertex buffers (+8 bytes/vert) | +~15 MB total |

**Recommendation**: Start with raw BGRA (simpler). If memory becomes an issue, add BC1 compression in Phase 5.

---

## Critical Implementation Notes

1. **ADT file naming**: `{MapName}_{A}_{B}.adt` where `A` maps to tileY, `B` maps to tileX. This is confirmed in MEMORY.md: `Azeroth_31_49.adt` has A=31 (tileY), B=49 (tileX).

2. **MCNK position at 0x68**: Stored as `(wowX, wowZ, wowY)`, NOT `(wowY, wowZ, wowX)`. Empirically confirmed.

3. **Alpha format is map-wide**: MPHD flags in WDT determine the format for ALL tiles. Azeroth = 4-bit packed.

4. **Layer 0 has no alpha**: Layer 0 is always the base texture, 100% opaque. Only layers 1-3 have MCAL alpha maps (when `flags & 0x100`).

5. **IFF tags are reversed**: In the binary file, "MCNK" is stored as bytes `{'K','N','C','M'}`. The Python parser uses `b'KNCM'` for searching.

6. **Texture tiling scale**: WoW tiles ground textures approximately 8 times per chunk (empirical). The exact scale may need visual tuning against the WoW client.

7. **UV coordinate orientation**: WoW X+ = north, Y+ = west. The atlas is laid out with chunk(0,0) at NW corner. UV(0,0) = NW, UV(1,1) = SE. Ensure V increases southward (decreasing wowX) and U increases eastward (decreasing wowY).

8. **Existing TerrainPipeline is shared with BuildingRenderer**: Changing the vertex layout affects buildings too. The safest approach is a **separate** `TerrainTexturePipeline` with the extended layout, keeping the original pipeline unchanged for buildings.

9. **StormLib thread safety**: `SFileOpenFileEx` + `SFileReadFile` + `SFileCloseFile` on the same archive handle is safe from multiple threads (each call opens its own file context). Verify with stress test.

10. **Decimation and UV**: UV coordinates must be generated AFTER decimation, mapping the subsampled vertex positions back to tile-normalized [0,1] range. The formula `u = (tileOriginY - v.y) / TILE_SIZE` works regardless of decimation level.

---

## Dependencies

| Dependency | Version | Purpose | Status |
|------------|---------|---------|--------|
| StormLib | 9.x | MPQ archive access | Already linked via vcpkg |
| BLP decoder | internal | BLP -> BGRA decode | Already implemented |
| MpqArchiveSet | internal | Multi-MPQ file reader | Already implemented |
| DX11 | 11.0 | GPU rendering | Already used |
| D3DCompiler | 47 | Shader compilation | Already used |
| glog | vcpkg | Logging | Already used |

No new external dependencies required.

---

## Implementation Order

```
Phase 1 ──> Phase 2 ──> Phase 3 ──> Phase 4 ──> Phase 5
  |            |            |            |            |
  ADT parser   CPU blend    UV + shader  Integration  UI + LOD
  MCAL decode  BLP cache    new pipeline worker ext.   settings
  WDT flags    atlas build  sampler      GPU cache    BC1 (opt)
```

All phases are sequential — each builds on the previous. Estimated total: ~2000-2500 lines of new C++ code.
