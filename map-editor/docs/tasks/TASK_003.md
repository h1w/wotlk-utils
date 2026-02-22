# TASK-003: Terrain Geometry Rendering (Input Mesh)

**Status**: DONE (All phases complete)
**Created**: 2026-02-23
**Last Updated**: 2026-02-23

---

## Overview

Add **terrain geometry rendering** to the map editor's 3D mode — render the actual world geometry (terrain heightmap + buildings/objects) as solid opaque meshes underneath the semi-transparent navmesh overlay. This produces a visualization similar to Recast Demo, where the "input mesh" (the triangle soup that Recast processes to generate navmesh) is visible alongside the navigation polygons.

**Goal**: Transform the current view (navmesh wireframe floating in empty space) into a Recast Demo-like view (solid grey terrain + buildings with colored navmesh overlay on top).

**Location**: `map-editor/` in the `wotlk-utils` repository.

**References**:
- Recast Demo screenshot (target look): semi-transparent colored navmesh over solid grey terrain/buildings
- https://drewkestell.us/Article/6/Chapter/20 — terrain extraction pipeline overview
- https://cutler.sg/2011/06/navigation-mesh-path-finding-in-mmorpg-bots — navmesh visualization context

---

## Phase Status Summary

| Phase | Description | Status |
|-------|-------------|--------|
| Phase 1 | Terrain heightmap loader + renderer (.map files) | **DONE** (2026-02-23) |
| Phase 2 | Buildings/objects geometry loader (Buildings/*.wmo/*.m2) | **DONE** (2026-02-23) |
| Phase 3 | VMap tile placement loader (.vmtile spawn placement) | **DONE** (2026-02-23) |
| Phase 4 | Navmesh transparency + render order integration | **DONE** (2026-02-23) |
| Phase 5 | Streaming, LOD, and polish | **DONE** (2026-02-23) |

---

## Architecture Overview

### Render Pipeline (after implementation)

```
1. Clear backbuffer + depth (dark background)
2. Render ground plane (minimap tiles at Z=0, depth write OFF)          [existing]
3. *** Render terrain heightmap (opaque solid grey, depth write ON) ***  [NEW — Phase 1]
4. *** Render building geometry (opaque solid grey, depth write ON) ***  [NEW — Phase 2+3]
5. Render navmesh (semi-transparent overlay, depth test ON, write OFF)   [existing, modified]
6. Render overlays (graph, routes, paths, grid, player)                  [existing]
7. ImGui panels on top                                                   [existing]
```

### Data Flow

```
.map files (TC terrain)         → TerrainLoader → TerrainRenderer (GPU mesh)
     ↓
Buildings/*.wmo/*.m2 (TC models) → BuildingLoader ─┐
     ↓                                              ├→ BuildingRenderer (GPU mesh)
.vmtile files (TC placements)   → VMapTileLoader ──┘
     ↓
.mmtile files (navmesh)         → TileCache → NavmeshRenderer3D (semi-transparent)  [existing]
```

### Data Sources

| Source | Path | Content |
|--------|------|---------|
| Terrain heightmaps | `trinitycore_data/maps/MMMXXYY.map` | V9 (129x129) + V8 (128x128) height grids per tile |
| Model geometry | `trinitycore_data/Buildings/*.wmo`, `*.m2` | Pre-extracted triangle meshes (VERT + INDX chunks) |
| Model placements | `trinitycore_data/vmaps/MMM_XX_YY.vmtile` | Which model goes where (position, rotation, scale) |
| VMap tree index | `trinitycore_data/vmaps/MMM.vmtree` | BVH tree for spatial lookup (optional, for culling) |

**Data directory**: `Z:\Games\wow 3.3.5a client\wotlk\trinitycore_data` (configurable via settings, same as mmaps path parent).

**Statistics**: 5744 `.map` files, 362 `.vmtile` files (EK alone), 1612 `.wmo` + 5938 `.m2` in Buildings.

---

## Phase 1: Terrain Heightmap Loader + Renderer

### Goal

Parse TrinityCore `.map` files and render the terrain heightmap as a solid grey 3D mesh. This alone provides the ground surface — hills, valleys, coastlines — visible beneath the navmesh.

### 1.1: .map File Format (TC Map Extractor Output)

**File naming**: `MMMXXYY.map` where `MMM` = mapId (3 digits, zero-padded), `XX` = tileX (2 digits), `YY` = tileY (2 digits).
Example: `0004830.map` = map 0 (Eastern Kingdoms), tileX=48, tileY=30 (Stormwind area).

**Tile coordinate formula** (same as navmesh):
```
tileX = 31 - floor(wowX / 533.33333)
tileY = 31 - floor(wowY / 533.33333)
```

#### File Header (`map_fileheader`, 44 bytes)

```
Offset  Size  Type      Field            Description
0x00    4     char[4]   mapMagic         "MAPS"
0x04    4     uint32    versionMagic     Format version (typically 0x0A = 10)
0x08    4     uint32    buildMagic       WoW build number
0x0C    4     uint32    areaMapOffset    Byte offset to AREA chunk
0x10    4     uint32    areaMapSize      Size of AREA chunk (0 = no area data)
0x14    4     uint32    heightMapOffset  Byte offset to MHGT chunk
0x18    4     uint32    heightMapSize    Size of MHGT chunk (0 = no height data)
0x1C    4     uint32    liquidMapOffset  Byte offset to MLIQ chunk
0x20    4     uint32    liquidMapSize    Size of MLIQ chunk (0 = no liquid data)
0x24    4     uint32    holesOffset      Byte offset to holes chunk
0x28    4     uint32    holesSize        Size of holes chunk (0 = no holes)
```

#### MHGT Chunk (Height Data)

**Header** (16 bytes at `heightMapOffset`):

```
Offset  Size  Type      Field            Description
0x00    4     char[4]   fourCC           "MHGT"
0x04    4     uint32    flags            Bit flags:
                                           0x01 = NO_HEIGHT (flat tile, use gridHeight)
                                           0x02 = HEIGHT_AS_INT16
                                           0x04 = HEIGHT_AS_INT8
                                           0x08 = HAS_FLIGHT_BOUNDS
0x08    4     float     gridHeight       Minimum height (base)
0x0C    4     float     gridMaxHeight    Maximum height
```

**Height data** (follows header, 16 bytes in):

| Flag | V9 grid | V8 grid | Data type | Total data bytes |
|------|---------|---------|-----------|-----------------|
| None (float) | 129x129 = 16641 floats | 128x128 = 16384 floats | float32 | 132,100 |
| 0x02 (int16) | 129x129 = 16641 uint16 | 128x128 = 16384 uint16 | uint16 | 66,050 |
| 0x04 (int8) | 129x129 = 16641 uint8 | 128x128 = 16384 uint8 | uint8 | 33,025 |
| 0x01 (none) | — | — | — | 0 (flat at gridHeight) |

**Decompression formula** (int16/int8 to float):
```cpp
// For int16:
float height = gridHeight + (uint16_value / 65535.0f) * (gridMaxHeight - gridHeight);
// For int8:
float height = gridHeight + (uint8_value / 255.0f) * (gridMaxHeight - gridHeight);
```

**Verified**: Stormwind tile `0004830.map` — heightMapSize=66066, flags=0x02 (int16), gridHeight~6.0, gridMaxHeight~185.0.

#### V9 and V8 Grid Layout

Each `.map` tile covers 533.33 yards. The terrain is subdivided into a grid:

- **V9** (outer vertices): 129 x 129 grid — vertex positions at cell corners
- **V8** (inner vertices): 128 x 128 grid — vertex positions at cell centers

Together they form a tessellated terrain mesh:

```
V9 ---- V9 ---- V9
|  \  / | \  /  |
|   V8  |  V8   |     V9 = outer vertex (corner)
|  /  \ | /  \  |     V8 = inner vertex (center)
V9 ---- V9 ---- V9
```

Each cell (bounded by 4 V9 corners + 1 V8 center) produces **4 triangles** (fan from center):
```
Triangle 1: V9[r][c],     V9[r][c+1],   V8[r][c]
Triangle 2: V9[r][c+1],   V9[r+1][c+1], V8[r][c]
Triangle 3: V9[r+1][c+1], V9[r+1][c],   V8[r][c]
Triangle 4: V9[r+1][c],   V9[r][c],     V8[r][c]
```

**Triangles per tile**: 128 x 128 x 4 = **65,536 triangles** (196,608 vertices before indexing).

**World position of each vertex**:
```cpp
// Tile origin in WoW coords:
float tileOriginX = (32 - tileX) * 533.33333f;   // north edge of tile
float tileOriginY = (32 - tileY) * 533.33333f;   // west edge of tile
float cellSize = 533.33333f / 128.0f;             // ~4.1667 yards per cell

// V9[row][col] position:
float wowX = tileOriginX - row * cellSize;        // X decreases southward
float wowY = tileOriginY - col * cellSize;        // Y decreases eastward
float wowZ = V9[row * 129 + col];                 // height from decompressed array

// V8[row][col] position (center of cell):
float wowX = tileOriginX - (row + 0.5f) * cellSize;
float wowY = tileOriginY - (col + 0.5f) * cellSize;
float wowZ = V8[row * 128 + col];
```

**IMPORTANT**: The V9 data is stored first (129x129 values), followed immediately by V8 data (128x128 values). Data is stored row-major: `V9[0][0], V9[0][1], ..., V9[0][128], V9[1][0], ...`.

#### Holes Data

If `holesSize > 0`, there is a 16x16 grid of `uint16` hole flags (512 bytes). Each bit in the uint16 represents a sub-cell. Holes indicate terrain cutouts (mine entrances, cave openings, etc.). Triangles in holed cells should be **skipped** during mesh generation to avoid rendering invisible ground.

### 1.2: TerrainLoader Class

**New file**: `src/data/terrain_loader.h`
**New file**: `src/data/terrain_loader.cpp`

```cpp
namespace mapedit {

struct TerrainTileData {
    int tileX, tileY;
    bool valid = false;

    // Decompressed height arrays (always float, regardless of storage format)
    float v9[129 * 129];   // outer vertices
    float v8[128 * 128];   // inner vertices

    // Holes: 16x16 grid, each uint16 is a bitmask of 4x4 sub-holes
    uint16_t holes[16 * 16];
    bool hasHoles = false;

    // Computed AABB
    float minZ, maxZ;
};

class TerrainLoader {
public:
    void SetDataPath(const std::string& tcDataPath);

    // Load a single terrain tile. Returns false if file missing or no height data.
    bool LoadTile(uint32_t mapId, int tileX, int tileY, TerrainTileData& out);

private:
    std::string m_dataPath;   // e.g. "Z:/Games/wow 3.3.5a client/wotlk/trinitycore_data"

    bool ParseMapFile(const std::string& path, TerrainTileData& out);
    void DecompressHeights(const uint8_t* raw, uint32_t flags,
                           float gridH, float gridMaxH,
                           float* v9, float* v8);
};

} // namespace mapedit
```

**Key implementation notes**:
- File path: `{dataPath}/maps/{mapId:03d}{tileX:02d}{tileY:02d}.map`
- If `heightMapSize == 0` or flag `NO_HEIGHT`, the tile is flat at `gridHeight`
- If flag `HAS_FLIGHT_BOUNDS` (0x08), skip the extra 72 bytes of flight bound data after height data (or just stop reading at `heightMapSize`)

### 1.3: Terrain Mesh Generation

**New file**: `src/data/terrain_mesh.h`
**New file**: `src/data/terrain_mesh.cpp`

Convert `TerrainTileData` into GPU-ready triangle mesh:

```cpp
namespace mapedit {

struct TerrainVertex {
    float x, y, z;    // WoW world coordinates
    float nx, ny, nz; // Normal vector (for lighting)
};

struct TerrainMesh {
    int tileX, tileY;
    std::vector<TerrainVertex> vertices;
    std::vector<uint32_t> indices;       // triangle indices (3 per tri)
    float minX, minY, minZ;
    float maxX, maxY, maxZ;
};

// Generate triangle mesh from terrain tile data.
// Optionally decimate (skip every N-th row/col) for LOD.
void GenerateTerrainMesh(const TerrainTileData& tile, int tileX, int tileY,
                          TerrainMesh& outMesh, int decimation = 1);

} // namespace mapedit
```

**Mesh generation algorithm**:

```cpp
void GenerateTerrainMesh(const TerrainTileData& tile, int tileX, int tileY,
                          TerrainMesh& out, int decimation) {
    float tileOriginX = (32 - tileX) * 533.33333f;
    float tileOriginY = (32 - tileY) * 533.33333f;
    float cellSize = 533.33333f / 128.0f;

    // Build vertex positions for V9 and V8
    // V9: 129x129 vertices at cell corners
    // V8: 128x128 vertices at cell centers
    // Total vertices: 129*129 + 128*128 = 33025

    int v9Offset = 0;
    int v8Offset = 129 * 129;

    for (int row = 0; row < 129; row++) {
        for (int col = 0; col < 129; col++) {
            TerrainVertex v;
            v.x = tileOriginX - row * cellSize;
            v.y = tileOriginY - col * cellSize;
            v.z = tile.v9[row * 129 + col];
            // Normal computed after all vertices placed (via cross product of edges)
            v.nx = v.ny = 0; v.nz = 1; // placeholder
            out.vertices.push_back(v);
        }
    }
    for (int row = 0; row < 128; row++) {
        for (int col = 0; col < 128; col++) {
            TerrainVertex v;
            v.x = tileOriginX - (row + 0.5f) * cellSize;
            v.y = tileOriginY - (col + 0.5f) * cellSize;
            v.z = tile.v8[row * 128 + col];
            v.nx = v.ny = 0; v.nz = 1;
            out.vertices.push_back(v);
        }
    }

    // Build index buffer: 4 triangles per cell (fan from V8 center)
    for (int row = 0; row < 128; row++) {
        for (int col = 0; col < 128; col++) {
            // Check holes
            if (tile.hasHoles) {
                int holeRow = row / 8;
                int holeCol = col / 8;
                int subRow = (row % 8) / 2;
                int subCol = (col % 8) / 2;
                uint16_t holeMask = tile.holes[holeRow * 16 + holeCol];
                if (holeMask & (1 << (subRow * 4 + subCol)))
                    continue;  // skip holed cell
            }

            uint32_t tl = v9Offset + row * 129 + col;           // top-left V9
            uint32_t tr = v9Offset + row * 129 + col + 1;       // top-right V9
            uint32_t bl = v9Offset + (row + 1) * 129 + col;     // bottom-left V9
            uint32_t br = v9Offset + (row + 1) * 129 + col + 1; // bottom-right V9
            uint32_t ct = v8Offset + row * 128 + col;           // center V8

            // 4 triangles (fan from center)
            out.indices.push_back(tl); out.indices.push_back(tr); out.indices.push_back(ct);
            out.indices.push_back(tr); out.indices.push_back(br); out.indices.push_back(ct);
            out.indices.push_back(br); out.indices.push_back(bl); out.indices.push_back(ct);
            out.indices.push_back(bl); out.indices.push_back(tl); out.indices.push_back(ct);
        }
    }

    // Compute normals per-vertex (average of adjacent face normals)
    ComputeVertexNormals(out);

    // Compute AABB
    ComputeAABB(out);
}
```

**Normal computation**: For each triangle, compute face normal via cross product of two edges. Accumulate at each vertex, then normalize. This gives smooth shading across the terrain.

**Decimation** (for LOD): Skip rows/cols by `decimation` factor. At decimation=2, use every 2nd row/col → 1/4 triangles. At decimation=4 → 1/16 triangles.

### 1.4: Terrain GPU Pipeline

**New file**: `src/render/terrain_pipeline.h`
**New file**: `src/render/terrain_pipeline.cpp`

The terrain pipeline is simpler than the navmesh pipeline — no barycentric wireframe, just solid shading.

```cpp
namespace mapedit {

struct TerrainVertexGpu {
    float x, y, z;       // Position (12 bytes)
    float nx, ny, nz;    // Normal (12 bytes)
};                        // 24 bytes total

struct TerrainCB {
    float viewProj[16];     // 64 bytes — View*Projection matrix
    float lightDir[4];      // 16 bytes — directional light direction (xyz), ambient (w)
    float baseColor[4];     // 16 bytes — terrain fill color RGBA
    float heightParams[4];  // 16 bytes — [0]=minZ, [1]=maxZ, [2]=colorMode, [3]=unused
};                           // 112 bytes

class TerrainPipeline {
public:
    bool Initialize(ID3D11Device* device);
    void Shutdown();

    ID3D11VertexShader*      GetVS() const;
    ID3D11PixelShader*       GetPS() const;
    ID3D11InputLayout*       GetLayout() const;
    ID3D11RasterizerState*   GetRastState() const;
    ID3D11DepthStencilState* GetDSState() const;
    ID3D11Buffer*            GetCB() const;
    bool IsReady() const;

private:
    ID3D11VertexShader*      m_vs = nullptr;
    ID3D11PixelShader*       m_ps = nullptr;
    ID3D11InputLayout*       m_layout = nullptr;
    ID3D11RasterizerState*   m_rastState = nullptr;
    ID3D11DepthStencilState* m_dsState = nullptr;
    ID3D11Buffer*            m_cb = nullptr;
};

} // namespace mapedit
```

**Pipeline state** (differences from navmesh):

| State | Terrain Pipeline | Navmesh Pipeline (existing) |
|-------|-----------------|---------------------------|
| Blend | **OPAQUE** (no blending) | Alpha blend (semi-transparent) |
| Depth write | **ON** | OFF (or bias — see Phase 4) |
| Depth test | LESS | LESS_EQUAL |
| Cull mode | BACK | NONE |
| Fill mode | SOLID | SOLID |

#### HLSL Shaders

**Vertex Shader**:
```hlsl
cbuffer TerrainCB : register(b0) {
    row_major float4x4 viewProj;
    float4 lightDir;      // xyz=direction, w=ambient
    float4 baseColor;     // RGBA
    float4 heightParams;  // x=minZ, y=maxZ, z=colorMode
};

struct VS_IN  { float3 pos : POSITION; float3 norm : NORMAL; };
struct VS_OUT {
    float4 clipPos  : SV_POSITION;
    float3 normal   : TEXCOORD0;
    float3 worldPos : TEXCOORD1;
};

VS_OUT main(VS_IN i) {
    VS_OUT o;
    o.clipPos  = mul(float4(i.pos, 1.0), viewProj);
    o.normal   = i.norm;
    o.worldPos = i.pos;
    return o;
}
```

**Pixel Shader**:
```hlsl
cbuffer TerrainCB : register(b0) {
    row_major float4x4 viewProj;
    float4 lightDir;
    float4 baseColor;
    float4 heightParams;
};

struct PS_IN {
    float4 clipPos  : SV_POSITION;
    float3 normal   : TEXCOORD0;
    float3 worldPos : TEXCOORD1;
};

float4 main(PS_IN i) : SV_TARGET {
    float3 N = normalize(i.normal);

    // Flip normal if facing away from light (two-sided terrain)
    float3 L = normalize(lightDir.xyz);
    float NdotL = dot(N, L);
    if (NdotL < 0.0) { N = -N; NdotL = -NdotL; }

    float ambient = lightDir.w;  // e.g. 0.3
    float lighting = ambient + (1.0 - ambient) * NdotL;

    // Color mode selection
    float3 color;
    int mode = (int)heightParams.z;

    if (mode == 0) {
        // Solid grey (Recast Demo style)
        color = baseColor.rgb;
    }
    else if (mode == 1) {
        // Height gradient: dark grey (low) → light grey (high)
        float t = saturate((i.worldPos.z - heightParams.x) /
                           max(heightParams.y - heightParams.x, 0.01));
        color = lerp(float3(0.25, 0.25, 0.28), float3(0.65, 0.65, 0.68), t);
    }
    else if (mode == 2) {
        // Slope shading: flat=grey, steep=dark
        float slope = 1.0 - abs(N.z);  // 0=flat, 1=vertical
        color = lerp(float3(0.5, 0.5, 0.53), float3(0.2, 0.2, 0.22), slope);
    }

    return float4(color * lighting, 1.0);  // fully opaque
}
```

### 1.5: TerrainRenderer Class

**New file**: `src/render/terrain_renderer.h`
**New file**: `src/render/terrain_renderer.cpp`

```cpp
namespace mapedit {

class TerrainRenderer {
public:
    bool Initialize(ID3D11Device* device, ID3D11DeviceContext* context);
    void Shutdown();

    void SetDataPath(const std::string& tcDataPath);

    // Update which terrain tiles should be loaded based on 3D camera position.
    // Loads/unloads tiles to match the viewport.
    void UpdateViewport(uint32_t mapId, float targetX, float targetY,
                        float cameraDistance);

    // Render all loaded terrain tiles.
    void Render(const Camera3D& camera, ID3D11RenderTargetView* rtv,
                ID3D11DepthStencilView* dsv);

    // Configuration
    int colorMode = 0;    // 0=solid grey, 1=height gradient, 2=slope
    bool enabled = true;

private:
    struct TileGpu {
        ID3D11Buffer* vb = nullptr;
        ID3D11Buffer* ib = nullptr;
        UINT indexCount = 0;
        float minX, minY, minZ;
        float maxX, maxY, maxZ;
    };

    using TileKey = std::pair<int, int>;

    void LoadTileGpu(uint32_t mapId, int tileX, int tileY);
    void ReleaseTileGpu(TileGpu& tile);
    bool FrustumTest(const float planes[6][4], const TileGpu& tile) const;

    ID3D11Device*        m_device = nullptr;
    ID3D11DeviceContext* m_context = nullptr;
    TerrainPipeline      m_pipeline;
    TerrainLoader        m_loader;

    std::map<TileKey, TileGpu> m_gpuCache;
    uint32_t m_currentMapId = UINT32_MAX;

    // Throttle GPU uploads: max N per frame
    static constexpr int kMaxUploadsPerFrame = 4;
};

} // namespace mapedit
```

**Loading strategy**: Same spiral-nearest-first pattern as the existing navmesh TileCache. Load radius proportional to `cameraDistance`. Max ~100 terrain tiles cached at once.

### 1.6: Integration into App

**File**: `src/app.h` — add members:
```cpp
TerrainRenderer m_terrainRenderer;
```

**File**: `src/app.cpp` — in `RenderFrame3D()`, add terrain render call BEFORE navmesh:
```cpp
// After clearing depth buffer and ground plane:
if (m_layerState.terrainEnabled) {
    m_terrainRenderer.UpdateViewport(m_currentMapId, targetX, targetY, camDist);
    m_terrainRenderer.Render(m_camera3d, m_rtv, m_dsv);
}
// Then navmesh (existing, now semi-transparent — see Phase 4):
m_navmeshRenderer3d.Render(m_camera3d, m_tileCache, m_rtv, m_dsv, ...);
```

**File**: `src/ui/layer_panel.h/.cpp` — add checkbox "Terrain" in the Layers panel.

**File**: `src/data/app_settings.h/.cpp` — add `terrainEnabled`, `terrainColorMode`, `tcDataPath` to persisted settings.

### 1.7: Files Changed/Created

```
NEW:  src/data/terrain_loader.h
NEW:  src/data/terrain_loader.cpp
NEW:  src/data/terrain_mesh.h
NEW:  src/data/terrain_mesh.cpp
NEW:  src/render/terrain_pipeline.h
NEW:  src/render/terrain_pipeline.cpp
NEW:  src/render/terrain_renderer.h
NEW:  src/render/terrain_renderer.cpp
MOD:  src/app.h              — add TerrainRenderer member
MOD:  src/app.cpp            — call terrain renderer in RenderFrame3D
MOD:  src/ui/layer_panel.h   — add Terrain checkbox
MOD:  src/ui/layer_panel.cpp
MOD:  src/data/app_settings.h  — add terrain settings
MOD:  src/data/app_settings.cpp
```

### 1.8: Verification

- Switch to 3D mode → enable Terrain layer → see solid grey terrain mesh with proper hills/valleys
- Orbit camera around Stormwind → terrain shape matches navmesh elevation
- Height gradient mode → lower terrain appears darker, higher terrain lighter
- Slope mode → cliffs appear darker than flat areas
- Holes in terrain visible (mine entrances, caves don't have solid ground)
- Frustum culling works — only visible tiles rendered
- Zoom out far → tiles stream in smoothly (max 4 GPU uploads per frame)
- Performance: 12 terrain tiles loaded at ~65K tri each = ~780K triangles → should render at 60fps easily

---

## Phase 2: Buildings/Objects Geometry Loader

### Goal

Parse pre-extracted model geometry from TrinityCore's `Buildings/` directory. These files contain collision-ready triangle meshes for WMO (buildings, bridges, caves) and M2 (trees, rocks, objects) models.

### 2.1: Buildings File Format (TC VMAP Output)

Files in `Buildings/` are written by TrinityCore's `vmap4_assembler`. They are NOT raw WoW WMO/M2 files — TC has already extracted the collision geometry into a simpler format.

**Magic**: `VMAP048\0` (8 bytes) — VMAP format version 4.8.

**File structure** (sequential chunks):

```
[VMAP048\0]  (8 bytes)      — magic + version
[Root header] (variable)     — root WMO info (bounds, flags, nGroups)
For each GroupModel:
  [GRP ]  (chunk)            — group header (flags, bounds, counts)
  [INDX]  (chunk)            — triangle indices (uint16[])
  [VERT]  (chunk)            — vertex positions (float[3][])
  [BIH tree] (optional)      — bounding interval hierarchy (skip for rendering)
  [Liquid] (optional)        — liquid geometry (optional, can skip)
```

#### Root Header (after 8-byte magic)

```
Offset  Size  Type        Field
0x00    4     uint32      rootWmoID (or nRootTriangles)
0x04    4     uint32      nGroupModels
0x08    4     uint32      flags
0x0C    24    float[6]    rootBounds (minX,minY,minZ, maxX,maxY,maxZ)
0x24    ?     padding
```

**NOTE**: The exact byte layout varies slightly between TC versions. The safest approach is to scan for `GRP ` chunk markers and parse each group independently.

#### GRP Chunk

```
"GRP " (4 bytes)
uint32 chunkSize
uint32 mogpFlags
uint32 nBatches          — number of render batches
For each batch (3 x uint32):
  uint32 startIndex
  uint32 nTriangles
  uint32 padding
```

#### INDX Chunk (Triangle Indices)

```
"INDX" (4 bytes)
uint32 chunkSize          — total bytes of index data
uint32 nIndices           — number of uint16 indices (nTriangles * 3)
uint16[nIndices]          — triangle vertex indices
```

#### VERT Chunk (Vertex Positions)

```
"VERT" (4 bytes)
uint32 chunkSize          — total bytes of vertex data
uint32 nVertices          — number of vertices
float[nVertices * 3]      — vertex positions (x, y, z for each vertex)
```

**Coordinate system**: Vertices are in **model-local coordinates** (WoW's Z-up coordinate system). They must be transformed by the spawn's position/rotation/scale from the `.vmtile` file.

#### Multiple Groups

Large WMOs (like Stormwind.wmo at 6.8 MB) contain many group models. Each `GRP`+`INDX`+`VERT` sequence is one group. Iterate through the file scanning for chunk tags.

### 2.2: BuildingLoader Class

**New file**: `src/data/building_loader.h`
**New file**: `src/data/building_loader.cpp`

```cpp
namespace mapedit {

struct BuildingMesh {
    std::string filename;     // e.g. "Stormwind.wmo"
    std::vector<float> vertices;    // flat array: x,y,z, x,y,z, ...
    std::vector<uint32_t> indices;  // triangle indices
    float bounds[6];          // AABB: minX, minY, minZ, maxX, maxY, maxZ
    bool valid = false;
};

class BuildingLoader {
public:
    void SetDataPath(const std::string& tcDataPath);

    // Load a building model from Buildings/ directory.
    // Caches internally — subsequent calls return cached data.
    const BuildingMesh* LoadBuilding(const std::string& filename);

    // Clear cache (on map change)
    void ClearCache();

private:
    std::string m_dataPath;
    std::unordered_map<std::string, BuildingMesh> m_cache;

    bool ParseVmapModel(const std::string& fullPath, BuildingMesh& out);
};

} // namespace mapedit
```

**Key implementation notes**:
- Scan file for `INDX` and `VERT` chunk markers; accumulate all groups into a single merged mesh
- Vertex offset tracking: each group's indices are local to its own vertex array; when merging, add a running vertex offset to indices
- Skip BIH tree data (used for raycast, not rendering) — scan past it by reading its header size
- M2 files in Buildings/ follow the same VMAP format (they've already been converted)

### 2.3: Files Changed/Created

```
NEW:  src/data/building_loader.h
NEW:  src/data/building_loader.cpp
```

### 2.4: Verification

- Load `Stormwind.wmo` → expect ~300K+ triangles of building geometry
- Load small models (lamp posts, crates) → small triangle counts
- Merged group geometry: no gaps between WMO groups
- Bounds are reasonable (not NaN, not degenerate)

---

## Phase 3: VMap Tile Placement Loader

### Goal

Parse `.vmtile` files to learn which models from `Buildings/` are placed in each map tile, with their world position, rotation, and scale. Then transform and render the building geometry at correct locations.

### 3.1: .vmtile File Format

**File naming**: `MMM_XX_YY.vmtile` where `MMM` = mapId (3 digits), `XX` = tileX, `YY` = tileY.
Example: `000_32_48.vmtile` = Eastern Kingdoms tile.

**Structure**:
```
[VMAP_4.8]    (8 bytes)     — magic
[numSpawns]   (4 bytes)     — uint32: number of model spawn entries
[spawn flags] (4 bytes)     — uint32: additional info
For each spawn:
  [spawnId]   (4 bytes)     — uint32: unique spawn ID
  [flags]     (2 bytes)     — uint16: spawn flags (MOD_HAS_BOUND=0x01, etc.)
  [adtId]     (4 bytes)     — uint32: ADT placement ID
  [mapId]     (4 bytes)     — uint32: map ID
  [modelName] (variable)    — null-terminated string (model filename in Buildings/)
  [position]  (12 bytes)    — float[3]: world X, Y, Z
  [rotation]  (12 bytes)    — float[3]: iRot (Euler angles or axis-angle)
  [bounds]    (24 bytes)    — float[6]: world AABB (if MOD_HAS_BOUND)
  [scale]     (4 bytes)     — float: uniform scale (only if version supports it)
```

**NOTE**: The exact spawn serialization can be read from TC source `ModelSpawn::readFromFile()`. The model name string is the key — it maps to a file in `Buildings/`.

**Alternative approach**: Instead of parsing the vmtile directly (fragile format), we can scan the vmtile for recognizable model filenames (they end in `.wmo` or `.m2` and are null-terminated) and extract their positions from adjacent float data. This is a fallback if the exact binary layout proves difficult.

### 3.2: VMapTileLoader Class

**New file**: `src/data/vmap_tile_loader.h`
**New file**: `src/data/vmap_tile_loader.cpp`

```cpp
namespace mapedit {

struct ModelSpawn {
    std::string modelName;     // filename in Buildings/
    float posX, posY, posZ;    // world position
    float rotX, rotY, rotZ;    // rotation (Euler angles)
    float scale;               // uniform scale
    float bounds[6];           // world AABB
};

struct VMapTileData {
    int tileX, tileY;
    std::vector<ModelSpawn> spawns;
    bool valid = false;
};

class VMapTileLoader {
public:
    void SetDataPath(const std::string& tcDataPath);

    // Load spawn placements for a map tile
    bool LoadTile(uint32_t mapId, int tileX, int tileY, VMapTileData& out);

private:
    std::string m_dataPath;
    bool ParseVmtile(const std::string& path, VMapTileData& out);
};

} // namespace mapedit
```

### 3.3: Transform Model to World Space

Each spawn's model geometry must be transformed:

```cpp
// For each vertex in the building mesh:
// 1. Scale
v *= spawn.scale;
// 2. Rotate (TC uses specific Euler angle order — verify from source)
v = RotateByEuler(v, spawn.rotX, spawn.rotY, spawn.rotZ);
// 3. Translate
v += Vector3(spawn.posX, spawn.posY, spawn.posZ);
```

**Rotation convention**: TC's `ModelSpawn` stores rotation as three floats. The exact interpretation (Euler XYZ, quaternion components, etc.) must be verified from TrinityCore source `VMapManager2::convertPositionToInternalRep()` or `ModelSpawn::GetTransformMatrix()`. This is the trickiest part of Phase 3.

**Practical approach**: Start by rendering models at correct positions WITHOUT rotation. Most small objects (lamp posts, barrels) will look okay. Add rotation correction iteratively by comparing visual placement with the navmesh underneath.

### 3.4: BuildingRenderer Class

**New file**: `src/render/building_renderer.h`
**New file**: `src/render/building_renderer.cpp`

```cpp
namespace mapedit {

class BuildingRenderer {
public:
    bool Initialize(ID3D11Device* device, ID3D11DeviceContext* context);
    void Shutdown();

    void SetDataPath(const std::string& tcDataPath);

    // Load building geometry for visible tiles
    void UpdateViewport(uint32_t mapId, float targetX, float targetY,
                        float cameraDistance);

    // Render all loaded building geometry
    void Render(const Camera3D& camera, ID3D11RenderTargetView* rtv,
                ID3D11DepthStencilView* dsv);

    bool enabled = true;

private:
    // GPU buffer for all buildings in a tile
    struct TileBuildings {
        ID3D11Buffer* vb = nullptr;
        ID3D11Buffer* ib = nullptr;
        UINT indexCount = 0;
        float minX, minY, minZ;
        float maxX, maxY, maxZ;
    };

    using TileKey = std::pair<int, int>;

    void LoadTileBuildings(uint32_t mapId, int tileX, int tileY);

    ID3D11Device*        m_device = nullptr;
    ID3D11DeviceContext* m_context = nullptr;
    TerrainPipeline      m_pipeline;  // Reuse terrain pipeline (same solid grey shading)
    BuildingLoader       m_buildingLoader;
    VMapTileLoader       m_vmapLoader;

    std::map<TileKey, TileBuildings> m_gpuCache;
};

} // namespace mapedit
```

**Strategy**: For each visible tile:
1. Load `.vmtile` → list of spawns
2. For each spawn, load Building model (cached)
3. Transform model vertices to world space
4. Merge all transformed meshes into a single VB+IB per tile
5. Upload to GPU, render with terrain pipeline (same grey shading)

**Merging per tile**: All buildings in one tile → one draw call per tile. This keeps draw call count manageable (~100 tiles × 1 call each).

### 3.5: Files Changed/Created

```
NEW:  src/data/vmap_tile_loader.h
NEW:  src/data/vmap_tile_loader.cpp
NEW:  src/render/building_renderer.h
NEW:  src/render/building_renderer.cpp
MOD:  src/app.h              — add BuildingRenderer member
MOD:  src/app.cpp            — call building renderer in RenderFrame3D
MOD:  src/ui/layer_panel.h   — add Buildings checkbox
MOD:  src/ui/layer_panel.cpp
```

### 3.6: Verification

- Enable Buildings layer → see building outlines at correct world positions
- Stormwind buildings match navmesh positions (cathedral, inn, auction house)
- No buildings floating in the air or underground (correct Z positioning)
- Bridges and tunnels connect properly to terrain
- Small objects (trees, rocks near pathways) visible
- Performance acceptable (~100 building draw calls + ~100 terrain draw calls = ~200 total)

---

## Phase 4: Navmesh Transparency + Render Order

### Goal

Make the navmesh semi-transparent so the terrain and buildings are visible underneath. Adjust depth buffer behavior to prevent z-fighting between navmesh and terrain.

### 4.1: Navmesh Alpha Adjustment

Currently the navmesh renders with `fillColor.a ≈ 0.9`. For the overlay effect:
- Set navmesh fill alpha to **0.4–0.5** (adjustable via Layer Panel slider)
- Set navmesh edge alpha to **0.7** (edges should be more visible)

**File**: `src/render/navmesh_renderer_3d.cpp` — adjust fill/edge color alpha.

### 4.2: Depth Buffer Strategy

The navmesh sits ~0.1–1.0 yards above the terrain (since Recast offsets by agent height). Without care, z-fighting occurs. Strategy:

1. **Terrain + Buildings**: Render with depth write **ON**, depth test **LESS** (opaque pass)
2. **Navmesh**: Render with depth write **OFF**, depth test **LESS_EQUAL** (transparent overlay)
3. **Overlays** (graph, routes, etc.): Render with depth write **OFF**, depth test **LESS_EQUAL**

The navmesh is naturally slightly above terrain due to Recast's voxelization. Combined with `LESS_EQUAL` test and depth write OFF, this avoids z-fighting without needing a depth bias.

If z-fighting still occurs, add a small **polygon offset** (depth bias):
```cpp
D3D11_RASTERIZER_DESC rd = {};
rd.DepthBias = -10;             // small negative bias = closer to camera
rd.SlopeScaledDepthBias = -1.0f;
rd.DepthBiasClamp = -0.001f;
```

### 4.3: Render Order (Final)

```cpp
void App::RenderFrame3D() {
    // 1. Clear
    ClearRTV(m_rtv, darkColor);
    ClearDSV(m_dsv);

    // 2. Ground plane (minimap, depth write OFF — background only)
    m_groundPlane3d.Render(...);

    // 3. OPAQUE PASS: Terrain (depth write ON)
    if (m_layerState.terrainEnabled)
        m_terrainRenderer.Render(m_camera3d, m_rtv, m_dsv);

    // 4. OPAQUE PASS: Buildings (depth write ON)
    if (m_layerState.buildingsEnabled)
        m_buildingRenderer.Render(m_camera3d, m_rtv, m_dsv);

    // 5. TRANSPARENT PASS: Navmesh (depth write OFF, alpha blend)
    if (m_layerState.navmeshEnabled)
        m_navmeshRenderer3d.Render(m_camera3d, m_tileCache, m_rtv, m_dsv, ...);

    // 6. OVERLAY PASS: Graph, routes, paths, grid, player
    m_primitives3d.BeginFrame();
    // ... accumulate primitives ...
    m_primitives3d.Flush(vpMatrix);

    // 7. ImGui (2D UI panels on top)
    ImGui::Render();
}
```

### 4.4: Layer Panel Updates

Add to the 3D mode section of Layer Panel:

```
[x] Terrain          [color mode: Solid Grey ▼]
    Alpha: [========|--] 1.0
[x] Buildings
[x] Navmesh
    Alpha: [====|------] 0.4
    [x] Wireframe Edges
    Color: [Height Gradient ▼]
[x] Grid
[x] Nodes
...
```

### 4.5: Files Changed/Created

```
MOD:  src/render/navmesh_pipeline_3d.cpp  — update DSState (depth write OFF for overlay)
MOD:  src/render/navmesh_renderer_3d.cpp  — alpha adjustment
MOD:  src/app.cpp                         — render order
MOD:  src/ui/layer_panel.h               — terrain/buildings toggles, alpha sliders
MOD:  src/ui/layer_panel.cpp
```

### 4.6: Verification

- Terrain visible under navmesh (grey mesh through blue/green navmesh)
- Buildings visible through navmesh
- No z-fighting between terrain and navmesh
- Alpha slider works: 0.0 = invisible navmesh, 1.0 = fully opaque
- Wireframe edges visible at navmesh alpha=0.4
- Toggling terrain/buildings/navmesh independently works
- Looks like Recast Demo screenshot

---

## Phase 5: Streaming, LOD, and Polish

### Goal

Optimize performance for large map areas and add quality-of-life features.

### 5.1: Background Loading

Terrain and building data loading should happen on a background thread, similar to `MinimapTileCache`:

```cpp
// Worker thread:
//   1. TerrainLoader::LoadTile() → TerrainTileData (disk I/O + decompression)
//   2. GenerateTerrainMesh() → TerrainMesh (CPU mesh generation)
//   3. Post result to main thread queue

// Main thread (per frame):
//   1. Dequeue completed meshes (max N per frame)
//   2. Upload VB/IB to GPU
//   3. Add to render cache
```

This prevents frame hitches when scrolling to new areas.

### 5.2: Terrain LOD

Three detail levels based on camera distance:

| Distance | Decimation | Triangles/tile | Use case |
|----------|-----------|---------------|----------|
| < 300 yards | 1 (full) | 65,536 | Close-up viewing |
| 300–800 yards | 2 | 16,384 | Mid-range |
| > 800 yards | 4 | 4,096 | Far distance |

Implement by keeping multiple VB/IB per tile or by generating on-demand based on camera distance.

### 5.3: Building LOD / Culling

- **Small object culling**: Skip M2 models smaller than a threshold (e.g. bounding box < 2 yards) when camera distance > 500 yards
- **Per-building frustum culling**: Use spawn bounds to skip off-screen buildings before transform
- **GPU instancing** (optional): If many identical models are placed, use instanced rendering

### 5.4: Terrain Tile Eviction

LRU cache for terrain GPU data. When cache exceeds ~200 tiles, evict oldest unused tiles. Same pattern as navmesh TileCache.

### 5.5: Liquid Rendering (Optional)

The `.map` files contain `MLIQ` chunks with water surface data. Could render as semi-transparent blue planes at the liquid height. Low priority — the navmesh already covers walkable water areas.

### 5.6: TC Data Path Configuration

Add a settings dialog or auto-detection for the TrinityCore data path:
- Check if parent directory of mmaps path contains `maps/` and `vmaps/` and `Buildings/`
- If found, auto-configure terrain data path
- Otherwise, show a directory picker in settings

### 5.7: Files Changed/Created

```
MOD:  src/render/terrain_renderer.h/.cpp   — background loading, LOD
MOD:  src/render/building_renderer.h/.cpp  — small object culling, LOD
MOD:  src/data/terrain_loader.h/.cpp       — thread-safe loading
MOD:  src/data/app_settings.h/.cpp         — TC data path configuration
MOD:  src/ui/layer_panel.h/.cpp            — LOD controls
```

---

## Complete File Inventory

### New Files (to create)

```
src/data/terrain_loader.h           — .map file parser
src/data/terrain_loader.cpp
src/data/terrain_mesh.h             — heightmap → triangle mesh conversion
src/data/terrain_mesh.cpp
src/data/building_loader.h          — Buildings/*.wmo/*.m2 parser
src/data/building_loader.cpp
src/data/vmap_tile_loader.h         — .vmtile placement parser
src/data/vmap_tile_loader.cpp
src/render/terrain_pipeline.h       — DX11 shaders/state for solid terrain
src/render/terrain_pipeline.cpp
src/render/terrain_renderer.h       — terrain tile GPU cache + render
src/render/terrain_renderer.cpp
src/render/building_renderer.h      — building geometry GPU cache + render
src/render/building_renderer.cpp
```

**Total new files**: 14 (7 .h + 7 .cpp)

### Modified Files

```
src/app.h                    — add TerrainRenderer, BuildingRenderer members
src/app.cpp                  — integrate into RenderFrame3D, render order
src/render/navmesh_pipeline_3d.cpp  — depth write OFF for transparent overlay
src/render/navmesh_renderer_3d.cpp  — alpha adjustment
src/ui/layer_panel.h/.cpp    — terrain/buildings toggles, alpha sliders
src/data/app_settings.h/.cpp — terrain settings persistence
```

**Total modified files**: ~8

### No Changes Needed

```
src/render/navmesh_pipeline_3d.h   — interface unchanged
src/render/primitives_3d.*          — overlay rendering unchanged
src/render/ground_plane_3d.*        — ground plane unchanged
src/camera/*                        — camera system unchanged
src/navmesh/*                       — navmesh loading unchanged
src/data/world_graph_data.*         — graph data unchanged
src/editor/*                        — editing unchanged
All 2D mode files                   — completely untouched
```

---

## Dependencies

No new vcpkg packages needed. All required libraries already available:
- **DirectXMath** — matrix/vector math (Windows SDK, header-only)
- **glog** — logging (already linked)
- Standard C++ file I/O — for reading .map/.vmtile/Buildings files

---

## Critical Implementation Notes

### 1. Coordinate system consistency

All terrain/building geometry uses WoW coordinates (X+=north, Y+=west, Z+=up). The existing Camera3D and view-projection matrix already handle WoW→DX11 coordinate mapping. No additional coordinate transforms needed in terrain/building shaders.

### 2. Tile coordinate alignment

Terrain `.map` tiles and navmesh `.mmtile` tiles use the **same tile coordinate system**:
```
tileX = 31 - floor(wowX / 533.33333)
tileY = 31 - floor(wowY / 533.33333)
```
But `.vmtile` files use a slightly different naming: `MMM_XX_YY.vmtile`. Verify that the XX/YY in vmtile filenames match the same tile coordinate convention.

### 3. Buildings file format fragility

The TC Buildings format has no formal spec and varies between TC versions. The safest parsing approach is **chunk-based scanning**: look for `INDX`/`VERT` chunk markers (4-byte ASCII tags) and read their `uint32 chunkSize` to navigate through the file. Don't rely on absolute offsets from the file header.

### 4. Memory budget

- Each terrain tile (65K tris): ~2.3 MB GPU (VB + IB with 32-bit indices)
- 100 terrain tiles loaded: ~230 MB GPU
- Each building tile: variable, ~1–10 MB depending on area density
- **Total estimate**: 300–500 MB GPU for a fully loaded area — acceptable for modern GPUs
- For memory-constrained systems, reduce max loaded tiles or increase LOD decimation

### 5. Performance expectations

- 100 terrain tiles × 65K tris = 6.5M triangles — well within DX11 capabilities
- Building geometry adds ~2–5M triangles depending on area
- Total ~10M triangles at 24 bytes/vertex ≈ 240 MB — single-digit ms GPU time
- Main bottleneck: disk I/O for initial loading → background thread solves this

### 6. X-flip in Camera3D

The existing Camera3D applies `XMMatrixScaling(-1,1,1)` to flip the X axis for correct east→right screen mapping. Terrain and building geometry uses the same coordinate space as navmesh, so this works automatically. Normals in the terrain pixel shader may need negation (same as navmesh — use `ddx/ddy` flat normals or negate per-vertex normals in the vertex shader if needed).

### 7. Incremental implementation

**Phase 1 alone** (terrain heightmap only) provides 80% of the visual improvement — the ground surface is the most important element. Buildings (Phases 2–3) add refinement but are optional for a first version. This means you can ship Phase 1 and iterate on the rest.

---

## Implementation Order (Recommended)

```
Phase 1 (terrain heightmap) ← START HERE — biggest visual impact
    ├── 1.2: TerrainLoader (.map parser)
    ├── 1.3: TerrainMesh (heightmap → triangles)
    ├── 1.4: TerrainPipeline (DX11 shaders)
    ├── 1.5: TerrainRenderer (GPU cache + render)
    └── 1.6: Integration into App + Layer Panel
         ↓
Phase 4 (navmesh transparency) ← Do this right after Phase 1
    ├── 4.1: Navmesh alpha adjustment
    ├── 4.2: Depth buffer strategy
    └── 4.3: Render order update
         ↓
Phase 2 (building geometry loader) ← Can be deferred
    ├── 2.2: BuildingLoader
    └── 2.3: Verification
         ↓
Phase 3 (vmtile placement) ← Can be deferred
    ├── 3.2: VMapTileLoader
    ├── 3.3: Transform to world space
    └── 3.4: BuildingRenderer
         ↓
Phase 5 (polish) ← As needed
    ├── 5.1: Background loading
    ├── 5.2: Terrain LOD
    └── 5.3–5.7: Culling, eviction, liquid, settings
```

Each phase is independently testable. The app remains fully functional after each phase.
