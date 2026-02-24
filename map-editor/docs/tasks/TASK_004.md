# TASK-004: Client-Side 3D Rendering (ADT Terrain + WMO Buildings + M2 Doodads)

**Status**: IN PROGRESS (Phase 1 complete)
**Created**: 2026-02-24
**Last Updated**: 2026-02-24

---

## Overview

Replace all server-side data sources (TrinityCore `.map`, `Buildings/`, `.vmtile`) with **WoW 3.3.5a client files** loaded directly from MPQ archives. This gives us full visual geometry for buildings (walls, roofs, interiors), textured terrain (grass, rock, dirt), and decorative props (trees, fences, lampposts).

**Why**: The current renderer uses TrinityCore collision geometry — simplified meshes designed for server-side LOS/pathfinding, not for visual rendering. Buildings are missing walls, terrains are untextured grey, and no props exist. Client files contain the complete visual data the WoW client uses to render the world.

**Location**: `map-editor/` in the `wotlk-utils` repository.

**Prerequisites**: MPQ archive reading (`mpq_archive.cpp`) and BLP texture decoding (`blp_decoder.cpp`) already implemented.

---

## Phase Status Summary

| Phase | Description | Status |
|-------|-------------|--------|
| Phase 1 | WMO visual geometry loader (buildings with walls!) | **COMPLETED** (2026-02-24) |
| Phase 2 | WMO textures & materials | NOT STARTED |
| Phase 3 | ADT terrain from client files | NOT STARTED |
| Phase 4 | Terrain texture splatting | NOT STARTED |
| Phase 5 | M2 doodad props | NOT STARTED |
| Phase 6 | ADT-based placements, remove TC dependency | NOT STARTED |
| Phase 7 | Water surfaces, LOD, polish | NOT STARTED |

---

## Architecture Overview

### Current Data Flow (TASK_003 — server-side)

```
TC .map files (heights only)       → TerrainLoader  → TerrainRenderer (grey mesh)
TC Buildings/*.wmo (COLLISION geom) → BuildingLoader → BuildingRenderer (grey collision mesh)
TC .vmtile (placements)            → VMapTileLoader ─┘
```

**Problems**: Buildings/ contains collision-only geometry (missing walls, no textures, rectangular slabs). The `.map` terrain is geometrically correct but untextured.

### New Data Flow (TASK_004 — client-side)

```
MPQ: *.adt (terrain chunks)        → AdtLoader      → TerrainRenderer (textured terrain)
MPQ: *.wmo root (materials)        → WmoVisualLoader → BuildingRenderer (textured buildings)
MPQ: *_NNN.wmo groups (visual geom)    ↑ per-batch with materials
MPQ: *.blp (textures)             → TextureCache    → D3D11 SRVs
MPQ: *.m2 + *.skin (doodads)      → M2Loader        → DoodadRenderer (props)
TC .vmtile (placements) [Phase 1-5]→ VMapTileLoader  → world positions  ← KEEP INITIALLY
MPQ: *.adt MODF/MDDF [Phase 6]    → AdtLoader       → world positions  ← REPLACE LATER
```

**Strategy**: Incremental replacement. Phase 1 replaces building GEOMETRY while keeping TC placement data. Each phase is independently useful. Phase 6 removes the TC dependency entirely.

### Render Pipeline (final)

```
1. Clear backbuffer + depth
2. Ground plane (minimap tiles at Z=0, depth write OFF)           [existing]
3. Terrain (textured, depth write ON)                              [Phase 3-4]
4. WMO buildings (textured, per-batch materials, depth write ON)   [Phase 1-2]
5. M2 doodads (textured, depth write ON)                           [Phase 5]
6. Navmesh (semi-transparent overlay, depth write OFF)             [existing]
7. Overlays (graph, routes, paths, grid, player)                   [existing]
8. ImGui panels                                                    [existing]
```

---

## Phase 1: WMO Visual Geometry Loader

### Goal

Load **full visual geometry** from WMO group files in MPQ. This replaces TC `Buildings/` collision meshes with complete WMO meshes that include all walls, roofs, floors, decorations — everything the WoW client renders.

**This is the highest-priority phase** — it directly fixes the "buildings have no walls" problem.

### 1.1: Existing Infrastructure to Reuse

| Component | File | What it provides |
|-----------|------|-----------------|
| MPQ reading | `mpq_archive.cpp` | `ReadFile(path)` → bytes, `ListFiles(pattern)` |
| WMO path resolution | `wmo_portal_loader.cpp` | `ResolveMpqPath()` maps TC model names → MPQ paths |
| WMO root parsing (partial) | `wmo_portal_loader.cpp` | Already parses MOHD (nGroups), MOPV, MOPT, MOPR |
| BLP texture decoding | `blp_decoder.cpp` | `Decode()` → BGRA pixels (for Phase 2) |
| Placement data | `vmap_tile_loader.cpp` | Position, rotation, scale from `.vmtile` |
| Building renderer | `building_renderer.cpp` | GPU cache, background loading, frustum culling |

### 1.2: WMO File Structure

A WMO consists of a **root file** and multiple **group files**:

```
World\wmo\Azeroth\Buildings\Stormwind\Stormwind.wmo        ← root (portals, materials, group info)
World\wmo\Azeroth\Buildings\Stormwind\Stormwind_000.wmo    ← group 0 (geometry)
World\wmo\Azeroth\Buildings\Stormwind\Stormwind_001.wmo    ← group 1
...
World\wmo\Azeroth\Buildings\Stormwind\Stormwind_NNN.wmo    ← group N
```

Group count `N` is read from MOHD chunk in the root file (already extracted by `wmo_portal_loader`).

**Group file naming**: strip `.wmo` from root path, append `_{NNN:03d}.wmo`.

### 1.3: WMO Group File Format

Each group file has a chunk-based structure. Chunks use **reversed 4-byte tags** (WoW convention):

| Chunk | Tag in file | Content | Needed |
|-------|-------------|---------|--------|
| MOGP | `PGOM` | Group header (68 bytes) — flags, bbox, portal refs, batch counts | YES |
| MOPY | `YPOM` | Per-triangle material info (2 bytes/tri) — flags, materialID | YES (Phase 2) |
| MOVI | `IVOM` | Vertex indices (`uint16[]`) — 3 per triangle | YES |
| MOVT | `TVOM` | Vertex positions (`float3[]`) — 12 bytes per vertex | YES |
| MONR | `RNOM` | Vertex normals (`float3[]`) — 12 bytes per vertex | YES |
| MOTV | `VTOM` | Texture coordinates (`float2[]`) — 8 bytes per vertex | YES (Phase 2) |
| MOBA | `ABOM` | Render batches (24 bytes each) — material binding | YES (Phase 2) |
| MOBN | `NBOM` | BSP tree nodes | skip |
| MOBR | `RBOM` | BSP tree face indices | skip |
| MOCV | `VCOM` | Vertex colors | optional |
| MOLR | `RLOM` | Light references | skip |
| MODR | `RDOM` | Doodad references | Phase 5 |

#### MOGP Header (68 bytes)

```
Offset  Size  Type       Field
0x00    4     uint32     groupNameOfs      (offset into root MOGN)
0x04    4     uint32     descNameOfs       (offset into root MOGN)
0x08    4     uint32     flags             (MOGP flags — exterior, interior, etc.)
0x0C    24    float[6]   boundingBox       (min xyz, max xyz — model local)
0x24    2     uint16     portalStart
0x26    2     uint16     portalCount
0x28    2     uint16     transBatchCount   (transparent batches)
0x2A    2     uint16     intBatchCount     (interior batches)
0x2C    2     uint16     extBatchCount     (exterior batches)
0x2E    2     uint16     padding
0x30    4     uint8[4]   fogIds
0x34    4     uint32     groupLiquid
0x38    4     uint32     uniqueID
0x3C    4     uint32     flags2
0x40    4     uint32     unused
```

**Key flags** (at offset 0x08):
- `0x8` — EXTERIOR (outdoor group)
- `0x2000` — INTERIOR (indoor lighting)
- `0x10000` — ALWAYSDRAW (always visible, e.g. outer shell)

#### MOVI — Vertex Indices

```
uint16_t indices[];    // 2 bytes each, 3 per triangle
```

Total count = chunkSize / 2.

#### MOVT — Vertex Positions

```
struct { float x, y, z; } vertices[];    // 12 bytes each
```

**Coordinate note**: TC's vmap4_extractor writes MOVT vertices **unchanged** (no coordinate swap). The (X, Z, -Y) conversion documented on wowdev.wiki applies to the WoW *client* rendering pipeline, NOT to TC-extracted data. Since we use TC `.vmtile` spawn positions (which are in TC VMAP space), the model vertices must also be in TC VMAP space — which they are, as-is from the file. **No coordinate swap needed.**

Total count = chunkSize / 12.

#### MONR — Vertex Normals

```
struct { float nx, ny, nz; } normals[];    // 12 bytes each, same count as MOVT
```

Same coordinate conversion as MOVT.

#### MOTV — Texture Coordinates (Phase 2)

```
struct { float u, v; } texcoords[];    // 8 bytes each, same count as MOVT
```

#### MOBA — Render Batches (Phase 2)

```
Offset  Size  Type     Field
0x00    12    int16[6] boundingBox (shorts)
0x0C    4     uint32   startIndex    (into MOVI)
0x10    2     uint16   count         (number of indices)
0x12    2     uint16   minVertex     (into MOVT)
0x14    2     uint16   maxVertex     (into MOVT)
0x16    1     uint8    flags
0x17    1     uint8    materialId    (index into root MOMT)
```

Total: 24 bytes per batch.

#### MOPY — Per-Triangle Material Info (Phase 2)

```
Offset  Size  Type    Field
0x00    1     uint8   flags       (bit 5 = RENDER, 0xFF = collision-only)
0x01    1     uint8   materialId  (index into root MOMT, 0xFF = no material)
```

Total: 2 bytes per triangle. **Triangles with `materialId == 0xFF` or without the RENDER flag (bit 5) should be skipped** — they are collision-only geometry invisible in the WoW client.

### 1.4: WmoVisualLoader Class

**New file**: `src/data/wmo_visual_loader.h`
**New file**: `src/data/wmo_visual_loader.cpp`

```cpp
namespace mapedit {

struct WmoGroupVisual {
    std::vector<float>    positions;    // flat: x,y,z per vertex (WMO local coords)
    std::vector<float>    normals;      // flat: nx,ny,nz per vertex
    std::vector<float>    texcoords;    // flat: u,v per vertex (Phase 2)
    std::vector<uint16_t> indices;      // triangle indices
    uint32_t mogpFlags = 0;             // group flags
    float    bbox[6] = {};              // model-local AABB

    // Per-triangle render info (Phase 2)
    struct TriInfo { uint8_t flags; uint8_t materialId; };
    std::vector<TriInfo> triInfo;       // MOPY data, 1 per triangle

    // Render batches (Phase 2)
    struct Batch {
        uint32_t startIndex;
        uint16_t indexCount;
        uint16_t minVertex, maxVertex;
        uint8_t  materialId;
        uint8_t  flags;
    };
    std::vector<Batch> batches;
};

struct WmoVisualData {
    bool valid = false;
    uint32_t nGroups = 0;
    std::vector<WmoGroupVisual> groups;

    // From root file (Phase 2):
    std::vector<std::string> textures;  // MOTX filenames
    struct Material {                   // MOMT entries
        uint32_t flags;
        uint32_t shader;
        uint32_t blendMode;
        uint32_t texture1Ofs;           // offset into MOTX → resolve to filename
        uint32_t texture2Ofs;
        uint32_t diffColor;             // BGRA
    };
    std::vector<Material> materials;
};

class WmoVisualLoader {
public:
    // Load full WMO visual data (root + all groups) from MPQ.
    // Uses WmoPortalLoader's path resolution to find MPQ paths.
    const WmoVisualData* Load(const std::string& vmapModelName,
                               const MpqArchiveSet& mpq);
    void ClearCache();

private:
    std::unordered_map<std::string, WmoVisualData> m_cache;

    bool ParseRootExtended(const std::vector<uint8_t>& data, WmoVisualData& out);
    bool ParseGroupFile(const std::vector<uint8_t>& data, WmoGroupVisual& out);

    // Path resolution — reuse pattern from WmoPortalLoader
    std::unordered_map<std::string, std::string> m_pathCache;
    std::unordered_map<std::string, std::string> m_basenameIndex;
    std::string ResolveMpqPath(const std::string& vmapName, const MpqArchiveSet& mpq);
};

} // namespace mapedit
```

### 1.5: Coordinate Conversion (WMO Local → WoW World) — IMPLEMENTED

**Key discovery**: TC's `vmap4_extractor` writes MOVT vertex positions **unchanged** from the WMO file. No coordinate swap is needed. The vertices are already in TC VMAP model-local space, which is the same space that `TransformVertices()` expects.

```cpp
// No coordinate swap — identity mapping from file
float mx = fileX * spawn.scale;
float my = fileY * spawn.scale;
float mz = fileZ * spawn.scale;

// Rotate (ZYX Euler, same matrix as existing TransformVertices)
float rx = r00 * mx + r01 * my + r02 * mz;
float ry = r10 * mx + r11 * my + r12 * mz;
float rz = r20 * mx + r21 * my + r22 * mz;

// Translate to WoW world (VMAP convention: VMAP_MID - pos)
float wowX = VMAP_MID - (spawn.posX + rx);
float wowY = VMAP_MID - (spawn.posY + ry);
float wowZ = spawn.posZ + rz;
```

Three coordinate swaps were tested during implementation:
1. `(X, -Z, Y)` — from wowdev.wiki — buildings upside down
2. `(Z, X, Y)` — from TC fixCoordSystem — buildings rotated wrong
3. `(X, Y, Z)` — identity — **CORRECT**, buildings align with navmesh

The wowdev.wiki (X, Z, -Y) conversion applies to the WoW *client* rendering pipeline. Since we use TC `.vmtile` placements (TC VMAP coordinate space), model vertices must match that space — and they already do, as-is from the file.

### 1.6: Integration into BuildingRenderer

Modify `building_renderer.cpp` worker to use `WmoVisualLoader` for WMO models:

```cpp
void BuildingRenderer::WorkerLoop() {
    BuildingLoader buildingLoader;       // fallback for M2 (still TC format until Phase 5)
    VMapTileLoader vmapLoader;
    WmoVisualLoader wmoVisualLoader;     // NEW: client-side WMO geometry
    WmoPortalLoader portalLoader;        // keep for portal culling

    // ...

    for (const auto& spawn : vmapData.spawns) {
        bool isWmo = !(spawn.flags & MOD_M2);

        if (isWmo && m_mpq && m_mpq->IsOpen()) {
            // Try loading visual geometry from MPQ
            const WmoVisualData* visual = wmoVisualLoader.Load(spawn.modelName, *m_mpq);
            if (visual && visual->valid) {
                // Use WMO visual geometry (full walls, normals, etc.)
                TransformWmoVisualVertices(*visual, spawn, mergedVerts, mergedIndices, ...);
                continue;  // skip TC fallback
            }
        }

        // Fallback: TC collision geometry (for models not found in MPQ)
        const BuildingMesh* mesh = buildingLoader.LoadBuilding(spawn.modelName);
        // ... existing code ...
    }
}
```

### 1.7: Phase 1 Rendering (Untextured)

For Phase 1, render WMO visual geometry with the **existing TerrainPipeline** (solid grey + directional lighting). The key improvement over TC collision geometry:

- **Proper vertex normals** from MONR → smooth lighting on curved surfaces
- **Complete geometry** — all walls, roofs, arches, staircases
- **No collision artifacts** — only renderable triangles (MOPY filter)

The existing `TerrainVertexGpu` format (position + normal, 24 bytes) is sufficient for Phase 1. Texture coordinates are loaded but unused until Phase 2.

### 1.8: MOPY Triangle Filtering

**Critical for clean rendering**: Skip collision-only triangles that the WoW client never draws.

```cpp
// When building index buffer from MOVI, filter by MOPY:
for (uint32_t tri = 0; tri < nTriangles; ++tri) {
    uint8_t flags = mopyData[tri * 2 + 0];
    uint8_t matId = mopyData[tri * 2 + 1];

    // Skip collision-only triangles (materialId 0xFF = invisible)
    if (matId == 0xFF) continue;

    // NOTE: The F_RENDER flag (0x20) check was REMOVED during implementation.
    // Many renderable triangles in real WMO files don't have this flag set.
    // Checking it caused half the walls to disappear. Only materialId == 0xFF
    // is a reliable indicator of collision-only geometry.

    // Include this triangle
    outIndices.push_back(moviData[tri * 3 + 0]);
    outIndices.push_back(moviData[tri * 3 + 1]);
    outIndices.push_back(moviData[tri * 3 + 2]);
}
```

### 1.9: Files Changed/Created

```
NEW:  src/data/wmo_visual_loader.h
NEW:  src/data/wmo_visual_loader.cpp
MOD:  src/render/building_renderer.h    — add WmoVisualLoader member
MOD:  src/render/building_renderer.cpp  — use WMO visual geometry in worker
```

### 1.10: Verification — COMPLETED

- [x] Stormwind buildings have **complete walls** (all districts visible)
- [x] Roofs, arches, staircases, towers — visible from every angle
- [x] Buildings align with navmesh overlay (correct world positioning)
- [ ] Smooth lighting on curved surfaces — DEFERRED (shader uses ddx/ddy flat normals; MONR data loaded but unused)
- [x] No collision-only rectangles visible (MOPY filtering: materialId == 0xFF only)
- [x] Performance: ~835K verts / ~785K tris per Stormwind tile, 10-18 fps at 12 tiles
- [x] Fallback: M2 models still render from TC Buildings/ (fallback path works)
- [x] Portal Culling ON + camera inside: BFS portal visibility works (rooms visible through doorways)
- [x] Portal Culling ON + camera outside: all groups rendered (shader wall hack disabled)
- [x] Mage Tower, Cathedral, Harbor Lighthouse — all fully rendered with geometry

---

## Phase 2: WMO Textures & Materials

### Goal

Add texture mapping to WMO buildings. Parse MOMT materials from root, load BLP textures via existing decoder, render per-batch with correct materials.

### 2.1: WMO Root — Extended Parsing

Extend `WmoVisualLoader::ParseRootExtended()` to also extract:

#### MOTX (tag `XTOM`) — Texture Filenames

Null-separated string block. Each string is a full MPQ path:
```
Textures\BaijuVillage\BaijuStonetile01.blp\0Textures\GenericStone\GenericStoneGrey01.blp\0...
```

Materials reference these by byte offset into the MOTX block.

#### MOMT (tag `TMOM`) — Materials (64 bytes each)

```
Offset  Size  Type     Field
0x00    4     uint32   flags          (F_UNLIT=0x1, F_UNFOGGED=0x2, F_TWOSIDED=0x4, ...)
0x04    4     uint32   shader         (shader type)
0x08    4     uint32   blendMode      (0=opaque, 1=alpha_key, 2=alpha, ...)
0x0C    4     uint32   texture1Ofs    (byte offset into MOTX for diffuse texture)
0x10    4     uint32   sidnColor      (emissive color BGRA)
0x14    4     uint32   frameSidnColor
0x18    4     uint32   texture2Ofs    (environment map / second texture)
0x1C    4     uint32   diffColor      (diffuse tint BGRA)
0x20    4     uint32   groundType
0x24    4     uint32   texture3Ofs
0x28    4     uint32   color2
0x2C    4     uint32   flags2
0x30    16    uint32[4] runtimeData   (zeros in file)
```

### 2.2: TextureCache Class

**New file**: `src/data/texture_cache.h`
**New file**: `src/data/texture_cache.cpp`

```cpp
namespace mapedit {

class TextureCache {
public:
    void Initialize(ID3D11Device* device, MpqArchiveSet* mpq);
    void Shutdown();

    // Get or create D3D11 SRV for a BLP texture path.
    // Returns nullptr if texture not found. Thread-safe for main thread only.
    ID3D11ShaderResourceView* Get(const std::string& mpqPath);

    void EvictUnused();   // release textures not accessed recently

private:
    struct Entry {
        ID3D11Texture2D* tex = nullptr;
        ID3D11ShaderResourceView* srv = nullptr;
        uint64_t lastAccess = 0;
    };

    ID3D11Device* m_device = nullptr;
    MpqArchiveSet* m_mpq = nullptr;
    std::unordered_map<std::string, Entry> m_cache;
    BlpDecoder m_decoder;

    bool LoadTexture(const std::string& path, Entry& out);
};

} // namespace mapedit
```

**Loading pipeline**: MPQ ReadFile → BLP Decode → D3D11 CreateTexture2D (BGRA) → CreateShaderResourceView.

**DXT support**: BLP files often use DXT1/DXT3/DXT5 compression. The existing `BlpDecoder` decodes to BGRA pixels. For better GPU performance, optionally upload DXT data directly as `DXGI_FORMAT_BC1/BC2/BC3` without CPU decompression (saves memory and decode time).

### 2.3: WMO Material Pipeline

**New file**: `src/render/wmo_pipeline.h`
**New file**: `src/render/wmo_pipeline.cpp`

New pixel shader with texture support:

```hlsl
cbuffer WmoCB : register(b0) {
    float4x4 viewProj;
    float4   lightDir;      // xyz=direction, w=ambient
    float4   matColor;      // material diffuse tint RGBA
    float4   matFlags;      // x=unlit, y=twoSided, z=alphaTest
};

Texture2D    texDiffuse : register(t0);
SamplerState samLinear  : register(s0);

struct PS_IN {
    float4 clipPos  : SV_POSITION;
    float3 normal   : TEXCOORD0;
    float3 worldPos : TEXCOORD1;
    float2 uv       : TEXCOORD2;
};

float4 main(PS_IN i) : SV_TARGET {
    float4 texColor = texDiffuse.Sample(samLinear, i.uv);

    // Alpha test (for vegetation, grates, etc.)
    if (matFlags.z > 0 && texColor.a < 0.5)
        discard;

    float3 N = normalize(i.normal);
    float3 L = normalize(lightDir.xyz);
    float NdotL = dot(N, L);
    if (NdotL < 0.0) { N = -N; NdotL = -NdotL; }

    float ambient = lightDir.w;
    float lighting = matFlags.x > 0 ? 1.0 : ambient + (1.0 - ambient) * NdotL;

    float3 color = texColor.rgb * matColor.rgb * lighting;
    return float4(color, texColor.a);
}
```

### 2.4: Per-Batch Rendering

Instead of one draw call per WMO group, use **one draw call per MOBA batch**:

```cpp
for (const auto& batch : group.batches) {
    // Bind material texture
    const WmoVisualData::Material& mat = wmoData.materials[batch.materialId];
    ID3D11ShaderResourceView* srv = m_textureCache.Get(mat.texture1Path);
    ctx->PSSetShaderResources(0, 1, &srv);

    // Update material constant buffer
    UpdateMaterialCB(mat);

    // Draw batch
    ctx->DrawIndexed(batch.indexCount, batch.startIndex, 0);
}
```

### 2.5: Vertex Format Extension

Extend GPU vertex format to include texture coordinates:

```cpp
struct WmoVertexGpu {
    float x, y, z;       // Position (12 bytes)
    float nx, ny, nz;    // Normal (12 bytes)
    float u, v;          // Texcoord (8 bytes)
};                        // 32 bytes total
```

### 2.6: Files Changed/Created

```
NEW:  src/data/texture_cache.h
NEW:  src/data/texture_cache.cpp
NEW:  src/render/wmo_pipeline.h
NEW:  src/render/wmo_pipeline.cpp
MOD:  src/data/wmo_visual_loader.h/.cpp — parse MOTX, MOMT from root
MOD:  src/render/building_renderer.h/.cpp — per-batch rendering, texture binding
MOD:  src/app.cpp — initialize TextureCache
```

### 2.7: Verification

- Stormwind buildings have stone, wood, tile textures
- Different materials per batch (walls ≠ roof ≠ floor)
- Alpha-tested materials work (grates, vegetation on buildings)
- Unlit materials (torches, glowing elements) render without shadow
- Two-sided materials render correctly
- Texture cache doesn't leak (eviction works)

---

## Phase 3: ADT Terrain from Client Files

### Goal

Replace TrinityCore `.map` terrain with WoW client **ADT files** loaded from MPQ. Geometrically identical to current terrain, but with pre-computed normals and texture layer data for Phase 4.

### 3.1: ADT File Location in MPQ

```
World\Maps\{InternalName}\{InternalName}_{Col}_{Row}.adt
```

Where `InternalName` comes from Map.dbc (already parsed by `dbc_reader.cpp`):
- mapId 0 → `Azeroth`
- mapId 1 → `Kalimdor`
- mapId 530 → `Expansion01`
- mapId 571 → `Northrend`

**Tile coordinate mapping** (TC convention → ADT filename):
```
ADT column (in filename) = tileY    (TC convention)
ADT row (in filename)    = tileX    (TC convention)
```
Example: Stormwind tile (tileX=48, tileY=30) → `Azeroth_30_48.adt`

**IMPORTANT**: Verify this mapping by testing — the convention may be `Azeroth_{tileX}_{tileY}.adt` instead. Check by loading a known tile and comparing heights with the existing .map terrain.

### 3.2: ADT File Structure

```
MVER  — version (18 for WotLK)
MHDR  — header with chunk offsets
MCIN  — 256 × MCNK offset/size entries (16 bytes each)
MTEX  — null-separated texture filenames
MMDX  — null-separated M2 model filenames
MMID  — M2 filename offsets into MMDX
MWMO  — null-separated WMO filenames
MWID  — WMO filename offsets into MWMO
MDDF  — M2 doodad placements (36 bytes each)
MODF  — WMO placements (64 bytes each)
MCNK[256] — terrain chunks (16×16 grid), each containing sub-chunks
```

### 3.3: MCNK Chunk Structure

Each MCNK covers 33.333 × 33.333 yards (1/16 of an ADT tile = 1/16 of 533.33).

**MCNK header** (128 bytes):

```
Offset  Size  Type      Field
0x00    4     uint32    flags
0x04    4     uint32    indexX           (0-15, column within ADT)
0x08    4     uint32    indexY           (0-15, row within ADT)
0x0C    4     uint32    nLayers          (texture layers, 0-4)
0x10    4     uint32    nDoodadRefs
0x14    4     uint32    ofsHeight        (MCVT offset relative to MCNK start)
0x18    4     uint32    ofsNormal        (MCNR offset)
0x1C    4     uint32    ofsLayer         (MCLY offset)
0x20    4     uint32    ofsRefs          (MCRF offset)
0x24    4     uint32    ofsAlpha         (MCAL offset)
0x28    4     uint32    sizeAlpha
0x2C    4     uint32    ofsShadow        (MCSH offset)
0x30    4     uint32    sizeShadow
0x34    4     uint32    areaId
0x38    4     uint32    nMapObjRefs
0x3C    2     uint16    holesLowRes      (16-bit hole mask)
0x3E    2     uint16    unknown
0x40    16    uint8[16] lowQualTexMap    (2-bit texture indices per 8×8 cell)
0x50    8     uint8[8]  noEffectDoodad
0x58    4     uint32    ofsSndEmitters
0x5C    4     uint32    nSndEmitters
0x60    4     uint32    ofsLiquid
0x64    4     uint32    sizeLiquid
0x68    12    float[3]  position         (world Y, world Z, world X — NOTE ORDER!)
0x74    4     uint32    ofsMCCV          (vertex colors)
0x78    4     uint32    ofsMCLV
0x7C    4     uint32    unused
```

**CRITICAL — Position field order**: The position at 0x68 is stored as `(Y, Z, X)` in WoW world coordinates. So: `position[0] = wowY, position[1] = wowZ (base height), position[2] = wowX`.

### 3.4: MCVT — Height Values (sub-chunk within MCNK)

Tag in file: `TVCM` (reversed "MCVT")

**145 floats** (580 bytes) in interleaved pattern:

```
Row 0 outer: v[0]..v[8]     (9 values — cell corners)
Row 0 inner: v[9]..v[16]    (8 values — cell centers)
Row 1 outer: v[17]..v[25]   (9 values)
Row 1 inner: v[26]..v[33]   (8 values)
...
Row 8 outer: v[136]..v[144] (9 values — last row of corners)
```

Heights are **relative to MCNK position Z** (base height).

**World position of each vertex**:
```cpp
// MCNK position (note the X/Y/Z swap in the header):
float chunkBaseX = mcnk.position[2];   // wowX
float chunkBaseY = mcnk.position[0];   // wowY
float chunkBaseZ = mcnk.position[1];   // wowZ (base height)

float unitSize = 533.33333f / 128.0f;  // ~4.1667 yards

// For outer vertex at (row, col) where row=0..8, col=0..8:
float wowX = chunkBaseX - row * unitSize;
float wowY = chunkBaseY - col * unitSize;
float wowZ = chunkBaseZ + mcvtHeights[outerIndex];

// For inner vertex at (row, col) where row=0..7, col=0..7:
float wowX = chunkBaseX - (row + 0.5f) * unitSize;
float wowY = chunkBaseY - (col + 0.5f) * unitSize;
float wowZ = chunkBaseZ + mcvtHeights[innerIndex];
```

**Triangle generation**: Same as current `GenerateTerrainMesh()` — 4 triangles per cell (fan from center V8 to 4 corner V9 vertices). 8×8 cells × 4 = 256 triangles per MCNK, 256 × 256 = 65,536 per ADT.

### 3.5: MCNR — Vertex Normals (sub-chunk within MCNK)

Tag in file: `RNCM` (reversed "MCNR")

**145 entries × 3 bytes** = 435 bytes + 13 bytes padding = 448 bytes:

```cpp
struct McnrNormal {
    int8_t x, z, y;   // NOTE: stored as (X, Z, Y) not (X, Y, Z)!
};
// Convert: normalX = x / 127.0f, normalY = y / 127.0f, normalZ = z / 127.0f
```

These replace the computed normals from `GenerateTerrainMesh()` — smoother and more accurate.

### 3.6: AdtLoader Class

**New file**: `src/data/adt_loader.h`
**New file**: `src/data/adt_loader.cpp`

```cpp
namespace mapedit {

struct AdtChunkData {
    float heights[145];       // MCVT (relative to base)
    float normals[145 * 3];   // MCNR → converted to float
    float baseX, baseY, baseZ; // MCNK position
    uint32_t indexX, indexY;
    uint16_t holes;
    uint32_t nLayers;

    // Texture layers (Phase 4)
    struct Layer {
        uint32_t textureIndex;  // into ADT MTEX
        uint32_t flags;
        uint32_t alphaOfs;
    };
    Layer layers[4];
};

struct AdtTileData {
    bool valid = false;
    int tileX, tileY;
    AdtChunkData chunks[256];   // 16×16 MCNK chunks

    // Texture and object names
    std::vector<std::string> textures;   // MTEX
    std::vector<std::string> m2Models;   // MMDX (Phase 5)
    std::vector<std::string> wmoModels;  // MWMO (Phase 6)

    // Placements (Phase 5-6)
    struct M2Placement { /* MDDF fields */ };
    struct WmoPlacement { /* MODF fields */ };
    std::vector<M2Placement> m2Placements;
    std::vector<WmoPlacement> wmoPlacements;

    // Alpha maps raw data (Phase 4)
    std::vector<uint8_t> alphaData;
};

class AdtLoader {
public:
    // Load ADT from MPQ. Requires map internal name (from Map.dbc).
    bool LoadTile(const std::string& mapInternalName, int tileX, int tileY,
                  const MpqArchiveSet& mpq, AdtTileData& out);

private:
    bool ParseAdt(const std::vector<uint8_t>& data, AdtTileData& out);
    bool ParseMcnk(const uint8_t* data, uint32_t size, AdtChunkData& out);
};

} // namespace mapedit
```

### 3.7: TerrainRenderer Modifications

Replace `TerrainLoader` (TC .map) with `AdtLoader` (MPQ ADT) in the worker thread:

```cpp
// Old:
TerrainTileData terrainData;
if (m_terrainLoader.LoadTile(mapId, tileX, tileY, terrainData)) { ... }

// New:
AdtTileData adtData;
if (m_adtLoader.LoadTile(mapInternalName, tileX, tileY, *m_mpq, adtData)) {
    // Generate mesh from ADT chunks (same V9+V8 pattern, but with client normals)
    GenerateTerrainMeshFromAdt(adtData, mesh);
}
// Fallback to TC .map if ADT not found
```

### 3.8: Files Changed/Created

```
NEW:  src/data/adt_loader.h
NEW:  src/data/adt_loader.cpp
MOD:  src/render/terrain_renderer.h/.cpp — use AdtLoader, pass map name
MOD:  src/data/terrain_mesh.h/.cpp — add GenerateTerrainMeshFromAdt()
MOD:  src/app.cpp — pass map internal name to terrain renderer
```

### 3.9: Verification

- Terrain geometry matches current .map terrain exactly (same heights)
- Lighting is smoother (ADT pre-computed normals vs computed normals)
- Holes work (mine entrances, cave openings)
- All maps load (Azeroth, Kalimdor, Northrend, Outland)
- Falls back to TC .map when ADT not in MPQ

---

## Phase 4: Terrain Texture Splatting

### Goal

Render terrain with multi-layer textures (grass, rock, dirt, sand) using alpha maps for blending, exactly like the WoW client.

### 4.1: Texture Layer System

Each MCNK chunk has up to 4 texture layers (MCLY):
- Layer 0: base texture (no alpha map — full opacity)
- Layers 1-3: overlay textures with per-pixel alpha blending

Alpha maps (MCAL) are 64×64 pixels per chunk, each pixel = 0-255 opacity.

### 4.2: Terrain Texture Atlas

Create a **texture array** (Texture2DArray) with all terrain textures for the loaded tiles:

```cpp
// For each unique terrain texture name from ADT MTEX:
// 1. Load BLP from MPQ via TextureCache
// 2. Add to texture array
// 3. Map texture index → array slice

ID3D11Texture2D* m_terrainTextureArray;  // 256×256 per slice, up to ~256 slices
```

### 4.3: Alpha Map Upload

Per-MCNK alpha maps → GPU texture (one per MCNK, or batched):

```cpp
// Per MCNK: 64×64 alpha texture, up to 3 layers (RGB channels)
// R = layer 1 alpha, G = layer 2 alpha, B = layer 3 alpha
// Layer 0 alpha = 1.0 - max(R, G, B) (base layer fills gaps)
```

### 4.4: Terrain Splatting Shader

```hlsl
Texture2DArray terrainTextures : register(t0);  // all terrain textures
Texture2D      alphaMap        : register(t1);   // 64×64 per chunk
SamplerState   samLinear       : register(s0);

cbuffer TerrainChunkCB : register(b1) {
    uint4 textureIndices;   // which array slices for layers 0-3
    uint  nLayers;
};

float4 main(PS_IN i) : SV_TARGET {
    float2 chunkUV = /* compute from world pos relative to chunk */;
    float2 tileUV  = chunkUV * 8.0;  // texture repeats ~8x per chunk

    // Sample alpha map
    float3 alpha = alphaMap.Sample(samLinear, chunkUV).rgb;

    // Sample and blend texture layers
    float3 color = terrainTextures.Sample(samLinear, float3(tileUV, textureIndices.x)).rgb;
    if (nLayers > 1) color = lerp(color, terrainTextures.Sample(..., textureIndices.y).rgb, alpha.r);
    if (nLayers > 2) color = lerp(color, terrainTextures.Sample(..., textureIndices.z).rgb, alpha.g);
    if (nLayers > 3) color = lerp(color, terrainTextures.Sample(..., textureIndices.w).rgb, alpha.b);

    // Lighting
    float lighting = computeLighting(i.normal, lightDir);
    return float4(color * lighting, 1.0);
}
```

### 4.5: Files Changed/Created

```
NEW:  src/render/adt_terrain_pipeline.h
NEW:  src/render/adt_terrain_pipeline.cpp
MOD:  src/render/terrain_renderer.h/.cpp — per-chunk rendering, alpha map upload
MOD:  src/data/adt_loader.h/.cpp — parse MCAL alpha data
MOD:  src/data/texture_cache.h/.cpp — texture array creation
```

### 4.6: Verification

- Terrain shows grass, rock, dirt, snow textures
- Blending between layers is smooth (no hard edges)
- Different terrain types visible (Elwynn forests, Dun Morogh snow, Westfall dry plains)
- Performance acceptable (texture array + alpha map per chunk)

---

## Phase 5: M2 Doodad Props

### Goal

Add static M2 models (trees, rocks, fences, lampposts, barrels, etc.) that populate the world.

### 5.1: M2 File Format (Static Geometry Only)

**M2 header** starts with magic `MD20`, version 264 (WotLK):

```
Offset  Size  Type       Field
0x00    4     char[4]    magic ("MD20")
0x04    4     uint32     version (264)
...
0x3C    4     uint32     nVertices
0x40    4     uint32     ofsVertices
...
```

**M2 vertex** (48 bytes):
```
float3   position;     // 12 bytes
uint8[4] boneWeights;  // 4 bytes (ignore for static)
uint8[4] boneIndices;  // 4 bytes (ignore for static)
float3   normal;       // 12 bytes
float2   texCoord0;    // 8 bytes
float2   texCoord1;    // 8 bytes
```

**Coordinate conversion**: M2 model space → WoW world: `wowX = fileX, wowY = -fileZ, wowZ = fileY` (same as WMO).

### 5.2: Skin Files (Index Data)

In WotLK, index data is in separate `.skin` files (same name, different extension):
```
{modelname}00.skin   ← LOD 0 (highest detail)
{modelname}01.skin   ← LOD 1
...
```

Skin file contains submeshes with index ranges and material bindings.

### 5.3: M2Loader Class

**New file**: `src/data/m2_loader.h`
**New file**: `src/data/m2_loader.cpp`

```cpp
namespace mapedit {

struct M2StaticMesh {
    std::vector<float> positions;    // flat x,y,z
    std::vector<float> normals;      // flat nx,ny,nz
    std::vector<float> texcoords;    // flat u,v
    std::vector<uint16_t> indices;
    std::string texturePath;         // primary diffuse texture
    float bounds[6];
    bool valid = false;
};

class M2Loader {
public:
    const M2StaticMesh* Load(const std::string& mpqPath, const MpqArchiveSet& mpq);
    void ClearCache();
private:
    std::unordered_map<std::string, M2StaticMesh> m_cache;
};

} // namespace mapedit
```

### 5.4: Placement Sources

M2 doodads are placed by two mechanisms:

1. **ADT MDDF** (world doodads — trees, rocks along roads):
   ```
   uint32 nameId;       // index into MMID → MMDX
   uint32 uniqueId;
   float3 position;     // world coords
   float3 rotation;     // degrees
   uint16 scale;        // 1024 = 1.0
   uint16 flags;
   ```

2. **WMO doodad sets** (props inside/around buildings — lamps, benches):
   - MODS (doodad set definitions) in WMO root
   - MODN (doodad M2 names) in WMO root
   - MODD (doodad placements: position, rotation, scale, color) in WMO root
   - MODR (doodad references per group) in WMO group files

### 5.5: Files Changed/Created

```
NEW:  src/data/m2_loader.h
NEW:  src/data/m2_loader.cpp
NEW:  src/render/doodad_renderer.h     (or integrate into BuildingRenderer)
NEW:  src/render/doodad_renderer.cpp
MOD:  src/data/adt_loader.h/.cpp — parse MDDF placements
MOD:  src/data/wmo_visual_loader.h/.cpp — parse MODS/MODN/MODD doodad sets
MOD:  src/app.cpp — integrate doodad renderer
MOD:  src/ui/layer_panel.cpp — doodad visibility toggle
```

### 5.6: Verification

- Trees along Elwynn roads
- Lampposts in Stormwind streets
- Fences, barrels, crates around buildings
- Correct scale and positioning (no floating/underground props)
- Small object culling at distance

---

## Phase 6: ADT-Based Placements — Remove TC Dependency

### Goal

Replace `.vmtile` spawn placements with ADT **MODF** (WMO) and **MDDF** (M2) placements. After this phase, the renderer uses **zero TrinityCore server files** — everything comes from MPQ.

### 6.1: WDT File (World Definition Table)

```
World\Maps\{InternalName}\{InternalName}.wdt
```

Contains a **MAIN** chunk: 64×64 grid of `uint32` flags indicating which ADT tiles exist. Use this instead of scanning for `.map` files.

```cpp
struct WdtTileFlags {
    uint32_t flags;   // bit 0 = ADT exists
};
// 64 * 64 = 4096 entries
```

### 6.2: MODF — WMO World Placements (64 bytes each)

```
uint32   nameId;        // index into MWID → MWMO
uint32   uniqueId;
float3   position;      // world coordinates
float3   rotation;      // degrees (Y, X, Z — heading, pitch, bank)
float3   extentsLo;     // world-space AABB min
float3   extentsHi;     // world-space AABB max
uint16   flags;
uint16   doodadSetIdx;
uint16   nameSet;
uint16   scale;         // 1024 = 1.0
```

**Position conversion**: ADT stores positions in `(17066.666 - wowX, wowZ, 17066.666 - wowY)` format. To convert:
```cpp
float wowX = 17066.666f - modf.position.x;   // or: 32 * 533.333 - pos.x
float wowY = 17066.666f - modf.position.z;
float wowZ = modf.position.y;
```

### 6.3: Migration Path

```
Phase 1-5:  vmtile placements (TC)  → world positions → WMO/M2 visual geometry (MPQ)
Phase 6:    ADT MODF/MDDF (MPQ)     → world positions → WMO/M2 visual geometry (MPQ)
```

After Phase 6, the only required input is the **WoW client Data directory** (or mounted MPQ archives).

### 6.4: Files Changed/Created

```
NEW:  src/data/wdt_loader.h
NEW:  src/data/wdt_loader.cpp
MOD:  src/render/building_renderer.cpp — use MODF placements from ADT
MOD:  src/render/terrain_renderer.cpp — use WDT for tile existence check
MOD:  src/data/adt_loader.h/.cpp — expose MODF/MDDF data
MOD:  src/app.cpp — remove TC data path dependency for 3D rendering
MOD:  src/data/app_settings.h/.cpp — WoW Data path instead of TC data path
```

### 6.5: Verification

- Remove TC data path configuration → 3D rendering still works from MPQ only
- All WMO placements match previous vmtile-based positions
- All M2 doodad placements correct
- WDT correctly identifies existing tiles (no 404 on missing ADTs)
- Maps with global WMO (dungeon instances like Deadmines) render correctly

---

## Phase 7: Water Surfaces, LOD, Polish

### Goal

Final polish pass: liquid rendering, distance-based LOD, performance optimization.

### 7.1: Water Surfaces

ADT MCNK chunks contain liquid data (ofsMCLQ/sizeMCLQ) with water surface heights. Render as semi-transparent blue planes:

```hlsl
// Water pixel shader
float4 main(PS_IN i) : SV_TARGET {
    float depth = saturate((waterHeight - terrainHeight) / 10.0);
    float3 color = lerp(float3(0.2, 0.3, 0.5), float3(0.05, 0.1, 0.3), depth);
    return float4(color, 0.6);  // semi-transparent
}
```

### 7.2: Terrain LOD

| Camera Distance | Strategy |
|----------------|----------|
| < 300 yards | Full detail (145 verts/chunk, 256 tris/chunk) |
| 300-800 yards | Decimate 2x (9×9 grid only, skip V8 centers) |
| > 800 yards | Decimate 4x (5×5 grid) |

### 7.3: WMO LOD

- Skip small WMO groups (bbox < threshold) at distance
- Simplify M2 doodads at distance (use lower skin LOD)
- Portal culling aggressiveness increases with distance

### 7.4: Texture Streaming

- Load terrain textures on demand (not all at once)
- Use mipmap level 2-3 for distant tiles (save VRAM)
- Async BLP decode on worker thread, D3D11 upload on main thread

### 7.5: Files Changed/Created

```
NEW:  src/render/water_renderer.h/.cpp
MOD:  src/render/terrain_renderer.cpp — LOD integration
MOD:  src/render/building_renderer.cpp — WMO LOD
MOD:  src/data/texture_cache.cpp — mipmap streaming
MOD:  src/ui/layer_panel.cpp — water toggle, LOD controls
```

---

## Complete File Inventory

### New Files

```
src/data/wmo_visual_loader.h/.cpp      — WMO root+group visual data parser (Phase 1)
src/data/texture_cache.h/.cpp          — BLP → D3D11 SRV texture cache (Phase 2)
src/render/wmo_pipeline.h/.cpp         — WMO textured rendering shaders (Phase 2)
src/data/adt_loader.h/.cpp             — ADT terrain parser from MPQ (Phase 3)
src/render/adt_terrain_pipeline.h/.cpp — terrain splatting shaders (Phase 4)
src/data/m2_loader.h/.cpp             — M2 static geometry parser (Phase 5)
src/render/doodad_renderer.h/.cpp     — M2 doodad rendering (Phase 5)
src/data/wdt_loader.h/.cpp            — WDT tile existence map (Phase 6)
src/render/water_renderer.h/.cpp      — liquid surface rendering (Phase 7)
```

**Total new files**: 18 (9 .h + 9 .cpp)

### Modified Files

```
src/render/building_renderer.h/.cpp  — WMO visual geometry, per-batch rendering
src/render/terrain_renderer.h/.cpp   — ADT-based terrain, texture splatting
src/data/terrain_mesh.h/.cpp         — adapt for ADT chunk data
src/data/wmo_portal_loader.h/.cpp    — possibly merge into wmo_visual_loader
src/app.h/.cpp                       — new renderers, TextureCache init
src/ui/layer_panel.h/.cpp            — texture/doodad/water toggles
src/data/app_settings.h/.cpp         — WoW Data path setting
```

### Deprecated After Phase 6

```
src/data/terrain_loader.h/.cpp       — replaced by adt_loader
src/data/building_loader.h/.cpp      — replaced by wmo_visual_loader
src/data/vmap_tile_loader.h/.cpp     — replaced by ADT MODF/MDDF
```

---

## Dependencies

### Existing (no changes needed)

- **StormLib** (MPQ reading) — already linked
- **DirectXMath** — matrix/vector math
- **glog** — logging
- **d3dcompiler** — runtime HLSL compilation

### Potentially New

- None required. BLP decoder already handles DXT1/DXT3/DXT5. All parsing is custom.
- **Optional**: DirectXTex for advanced DXT handling (e.g., generating mipmaps from non-DXT BLPs). Can be added via vcpkg if needed.

---

## Critical Implementation Notes

### 1. Coordinate Systems

Three coordinate spaces in play:

| Space | Convention | Used by |
|-------|-----------|---------|
| WoW World | X+=north, Y+=west, Z+=up | Camera, navmesh, final rendering |
| WMO/M2 Model | X, Z, -Y (file → wow: swap Y↔Z, negate) | WMO group files, M2 files |
| ADT Chunk | Position stored as (Y, Z, X) in header | MCNK position field |

**Test strategy**: After each coordinate conversion, overlay the result with the navmesh (which is correctly positioned). Any misalignment reveals coordinate bugs.

### 2. WMO Chunk Tag Byte Order

All WoW chunk tags are stored **reversed** in the file:
- `MOHD` appears as bytes `D`, `H`, `O`, `M` → compare with `memcmp(ptr, "DHOM", 4)`
- `MOVT` appears as `T`, `V`, `O`, `M` → compare with `memcmp(ptr, "TVOM", 4)`
- Same pattern already used in `wmo_portal_loader.cpp`

### 3. Thread Safety

- MPQ reading (`MpqArchiveSet::ReadFile`) is main-thread only (StormLib limitation)
- **Worker thread pattern**: Pre-read all needed MPQ files on main thread, pass bytes to worker for parsing
- Alternative: Serialize MPQ reads with a mutex (slower but simpler)
- D3D11 texture creation must happen on main thread

### 4. Memory Budget Estimate

| Data | Size per tile | 100 tiles |
|------|--------------|-----------|
| ADT terrain mesh | ~2.5 MB (65K tri × 32 bytes/vert) | 250 MB |
| WMO visual geometry | ~5-20 MB (varies by area) | 500 MB-2 GB |
| Terrain textures | ~4 MB (4 layers × 256×256 DXT) | 400 MB |
| WMO textures | ~2-10 MB per unique texture | ~200 MB |
| M2 doodads | ~1-5 MB per tile | ~300 MB |
| **Total estimate** | | **~1-3 GB GPU** |

For memory-constrained systems: reduce max tiles, use lower mip levels, skip M2 doodads.

### 5. Incremental Fallback

Each phase adds a fallback to the previous data source:
```cpp
// Try client data first, fall back to TC data
const WmoVisualData* visual = wmoLoader.Load(name, mpq);
if (visual) {
    RenderWmoVisual(*visual, ...);
} else {
    const BuildingMesh* collision = buildingLoader.LoadBuilding(name);
    RenderCollisionMesh(*collision, ...);  // old TC path
}
```

This ensures the editor remains functional during incremental implementation.

### 6. Portal Culling Reuse

The existing `WmoPortalLoader` (MOPV, MOPT, MOPR) provides portal culling data that works with the new WMO visual geometry. No changes needed — the portal graph is independent of which geometry we render. The per-group visibility (BFS from camera group) applies to visual groups the same way.

---

## Implementation Order (Recommended)

```
Phase 1 (WMO visual geometry) ← START HERE — fixes building walls!
    ├── 1.4: WmoVisualLoader (group file parser)
    ├── 1.5: Coordinate conversion (WMO local → WoW world)
    ├── 1.6: Integration into BuildingRenderer worker
    ├── 1.8: MOPY triangle filtering
    └── 1.10: Verification (buildings have walls!)
         ↓
Phase 2 (WMO textures) ← Biggest visual upgrade after Phase 1
    ├── 2.1: Root MOTX/MOMT parsing
    ├── 2.2: TextureCache (BLP → D3D11 SRV)
    ├── 2.3: WMO material pipeline (new shader)
    └── 2.4: Per-batch rendering
         ↓
Phase 3 (ADT terrain) ← Can be done in parallel with Phase 2
    ├── 3.6: AdtLoader (MCNK → heights + normals)
    ├── 3.7: TerrainRenderer adaptation
    └── 3.9: Verification (terrain matches existing)
         ↓
Phase 4 (terrain textures)
    ├── 4.1: MCLY + MCAL parsing
    ├── 4.2: Texture array creation
    └── 4.4: Splatting shader
         ↓
Phase 5 (M2 doodads) ← Lower priority, adds detail
    ├── 5.3: M2Loader
    ├── 5.4: MDDF/MODD placements
    └── 5.6: Verification
         ↓
Phase 6 (remove TC dependency) ← Final cleanup
    ├── 6.1: WDT loader
    ├── 6.2: MODF/MDDF from ADT replaces vmtile
    └── 6.3: Migration
         ↓
Phase 7 (polish) ← As needed
    ├── 7.1: Water
    ├── 7.2: Terrain LOD
    └── 7.4: Texture streaming
```

**Phase 1 alone** fixes the core problem (buildings without walls) and can be completed independently. Each subsequent phase improves visual quality incrementally. The editor remains fully functional after each phase.
