# TASK-011: Rendering Performance Optimization (Terrain Textures + Overlay Geometry)

**Status**: DONE
**Created**: 2026-02-26
**Completed**: 2026-02-26

---

## Overview

Optimize the two heaviest rendering subsystems in the map editor:

1. **Terrain texture rendering** — currently uses uncompressed 4 MB BGRA textures per tile (up to 600 MB VRAM at 150-tile cache), no mipmaps, per-tile constant buffer updates, CPU-side compositing at 100-200ms per tile.

2. **Overlay geometry (graph nodes/edges)** — currently rebuilds all 3D vertex data every frame from scratch, no frustum culling in 3D mode, no geometry caching between frames.

**Goal**: Reduce frame time for both subsystems by **5-10x combined**, reduce terrain VRAM by **4-8x**, and reduce tile load time by **~50%** (BC1 compress is cheaper than full BGRA upload).

**Location**: `map-editor/` in the `wotlk-utils` repository.

---

## Phase Status Summary

| Phase | Description | Status |
|-------|-------------|--------|
| Phase 1 | BC1 texture compression + mipmaps | DONE |
| Phase 2 | Texture Array batching | DONE |
| Phase 3 | 3D overlay frustum culling | DONE |
| Phase 4 | Graph geometry caching (dirty flag + static VB) | DONE |
| Phase 5 | Node instancing + CB batching | DONE |

---

## Current Architecture (Before Optimization)

### Terrain Texture Pipeline

```
Worker thread (per tile):
  ADT file (MPQ) --> AdtTextureParser --> MTEX + MCLY + MCAL
  BLP textures (MPQ) --> DecodeBlp() --> BGRA pixels (cached in BlpTextureCache)
  MCAL alpha --> mcal_decoder --> 64x64 uint8 alpha maps
  CPU compositor --> CompositeChunk() x 256 --> CompositeTileAtlas() --> 1024x1024 BGRA (4 MB)

Main thread (per tile upload):
  UploadToGpu():
    CreateTexture2D(B8G8R8A8_UNORM, 1024x1024, MipLevels=1)  --> 4 MB VRAM
    CreateShaderResourceView()

Per frame:
  For each visible tile (10-20 typical):
    Map/Unmap constant buffer (heightParams.z = colorMode)
    PSSetShaderResources(0, 1, &tileSRV)
    DrawIndexed(tile.indexCount)
```

**Current resource usage**:
- Per tile: VB ~534 KB + IB ~262 KB + Texture 4 MB = **~4.8 MB**
- Cache max: 150 tiles = **720 MB VRAM** (with textures)
- Compositor: ~100-200ms CPU per tile on worker thread
- Per-frame GPU: 10-20 draw calls + 10-20 CB updates + 10-20 SRV binds

### Overlay (Graph) Pipeline

```
Per frame (3D mode):
  Primitives3D::BeginFrame()     --> m_lines.clear()
  Graph3DRenderer::Render():
    For each edge (all edges, no culling):
      prims.AddLine(from, to, color)        --> push 2 vertices
    For each node (all nodes, no culling):
      prims.AddCircle(pos, radius, color, 16)  --> push 32 vertices
  [+ issue overlay, routes, paths, player marker]
  Primitives3D::Flush():
    Map VB (WRITE_DISCARD), memcpy m_lines, Unmap
    Single Draw(count, 0)

Per frame (2D mode):
  GraphRenderer::Render():
    BeginGraphOverlay() -> ImGui transparent window
    For each edge: WorldToScreen x2 + AddLine        (NO frustum cull on edges)
    For each node: frustum cull + WorldToScreen + AddCircleFilled + AddCircle
    EndGraphOverlay()
  GraphRenderer::RenderRoadOverlay():   --> separate ImGui window
  GraphRenderer::RenderIssueOverlay():  --> separate ImGui window
```

**Current resource usage**:
- Primitives3D VB: 8 MB static allocation (512K vertices max)
- Per-frame CPU: O(nodes + edges) loops, no caching, full rebuild
- Typical: 1000 nodes x 32 verts = 32K verts + 1500 edges x 2 verts = 3K verts = ~35K verts/frame
- 2D: separate ImGui windows per overlay = extra draw command overhead

---

## Key Files

### Terrain Texture System

```
src/data/terrain_texture_compositor.h    -- BlpTextureCache + CompositeTileAtlas/CompositeChunk
src/data/terrain_texture_compositor.cpp  -- CPU alpha-blending, 4-layer compositing (174 lines)
src/data/adt_texture_parser.h            -- AdtTextureData, AdtChunkTexture, WdtInfo
src/data/adt_texture_parser.cpp          -- ADT IFF parsing, MPHD reader (325 lines)
src/data/mcal_decoder.h                  -- Decode4Bit/8Bit/RLE alpha maps
src/data/mcal_decoder.cpp                -- Alpha decoders (83 lines)
src/data/terrain_mesh.h                  -- TerrainVertex (32 bytes: pos+norm+uv)
src/data/terrain_mesh.cpp                -- GenerateTerrainMesh(), LOD, UV generation (175 lines)
src/render/terrain_texture_pipeline.h    -- DX11 textured terrain pipeline
src/render/terrain_texture_pipeline.cpp  -- HLSL shaders, input layout, blend/depth states (235 lines)
src/render/terrain_pipeline.h            -- TerrainCB struct (viewProj, lightDir, baseColor, heightParams)
src/render/terrain_renderer.h            -- TileGpu, kMaxCachedTiles=150, LoadResult, async (154 lines)
src/render/terrain_renderer.cpp          -- WorkerLoop, UploadToGpu, Render, frustum cull (603 lines)
src/mpq/blp_decoder.h                   -- DecodeBlp() -> BlpImage
src/mpq/blp_decoder.cpp                 -- DXT1/3/5 + paletted decompression (240 lines)
```

### Overlay System

```
src/render/primitives_3d.h              -- LineVertex (16 bytes: pos+color), kMaxVertices=524288
src/render/primitives_3d.cpp            -- BeginFrame, AddLine, AddCircle, Flush (305 lines)
src/render/graph_renderer.h             -- GraphRenderer (2D), LayerVisibility
src/render/graph_renderer.cpp           -- 2D node/edge/issue/road rendering (290 lines)
src/render/graph_renderer_3d.h          -- Graph3DRenderer (3D)
src/render/graph_renderer_3d.cpp        -- 3D node/edge/issue rendering (213 lines)
src/render/route_renderer.cpp           -- 2D routes (102 lines)
src/render/route_renderer_3d.cpp        -- 3D routes (98 lines)
src/render/path_renderer.cpp            -- 2D path test (138 lines)
src/render/path_renderer_3d.cpp         -- 3D path test (78 lines)
src/render/frame_profiler.h             -- Layer enum {Terrain,Buildings,Navmesh,Overlays}, 60-frame avg
```

### Integration Points

```
src/app.cpp                              -- RenderFrame2D() line 799, RenderFrame3D() line 1126
src/app.h                                -- m_terrainRenderer, m_primitives3d, m_graphRenderer3d
src/render/camera3d.h                    -- Camera3D::GetFrustumPlanes(), WorldToScreen()
src/data/app_settings.h                  -- Settings persistence
src/ui/layer_panel.cpp                   -- Layer toggle checkboxes
```

---

## Phase 1: BC1 Texture Compression + Mipmaps

### Goal

Replace uncompressed `B8G8R8A8_UNORM` (4 MB/tile) with `BC1_UNORM` (~0.5 MB/tile) and add mipmap generation. This is the single highest-impact optimization: **8x VRAM reduction** + **2-4x texture sampling speedup** on GPU.

### 1.1: Add BC1 Compressor

**Why BC1 (DXT1)**: Terrain composite textures are opaque (alpha=255 always, set in `CompositeChunk`). BC1 compresses 4x4 pixel blocks into 8 bytes (vs 64 bytes BGRA) = 8:1 ratio. Hardware decompression is free.

**Library choice**: Use `stb_dxt.h` (single-header, public domain) — already in the style of the project (stb-style headers). Alternative: DirectXTex `Compress()` but heavier dependency.

**New file**: `src/data/bc1_compressor.h` / `.cpp`

```cpp
// Compress a 1024x1024 BGRA image into BC1 blocks
// Input:  bgra[1024*1024*4] = 4,194,304 bytes
// Output: bc1[1024*1024/2]  =   524,288 bytes (0.5 MB)
//
// Also generates mipmaps (10 levels: 1024, 512, 256, ... 1)
// Total BC1 with mips: ~699 KB per tile
struct CompressedAtlas {
    std::vector<uint8_t>  bc1Data;       // All mip levels concatenated
    std::vector<uint32_t> mipOffsets;    // Byte offset of each mip level
    std::vector<uint32_t> mipSizes;     // Byte size of each mip level
    uint32_t              width  = 1024;
    uint32_t              height = 1024;
    uint32_t              mipCount = 0;
};

CompressedAtlas CompressToBC1WithMips(const uint8_t* bgra, uint32_t width, uint32_t height);
```

**Mipmap generation** (CPU-side, before compression):
1. Start with 1024x1024 BGRA
2. Box-filter downsample to 512x512, 256x256, ... 1x1 (10 levels)
3. Compress each level to BC1
4. Concatenate all levels into single buffer with offset table

**Performance**: BC1 compression of 1024x1024 via stb_dxt takes ~5-15ms (much less than current compositor 100-200ms). Net impact on tile load time: negligible.

### 1.2: Modify UploadToGpu for BC1

**File**: `terrain_renderer.cpp`, function `UploadToGpu()` (line 196)

**Current** (line ~220):
```cpp
D3D11_TEXTURE2D_DESC td = {};
td.Width = 1024;
td.Height = 1024;
td.MipLevels = 1;
td.ArraySize = 1;
td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
td.SampleDesc.Count = 1;
td.Usage = D3D11_USAGE_IMMUTABLE;
td.BindFlags = D3D11_BIND_SHADER_RESOURCE;

D3D11_SUBRESOURCE_DATA srd = {};
srd.pSysMem = result.compositeAtlas.data();
srd.SysMemPitch = 1024 * 4;

m_device->CreateTexture2D(&td, &srd, &tex);
```

**New**:
```cpp
D3D11_TEXTURE2D_DESC td = {};
td.Width = atlas.width;
td.Height = atlas.height;
td.MipLevels = atlas.mipCount;                 // 10 levels
td.ArraySize = 1;
td.Format = DXGI_FORMAT_BC1_UNORM;             // <-- BC1 compressed
td.SampleDesc.Count = 1;
td.Usage = D3D11_USAGE_IMMUTABLE;
td.BindFlags = D3D11_BIND_SHADER_RESOURCE;

// One D3D11_SUBRESOURCE_DATA per mip level
std::vector<D3D11_SUBRESOURCE_DATA> initData(atlas.mipCount);
for (uint32_t mip = 0; mip < atlas.mipCount; ++mip) {
    uint32_t mipW = std::max(1u, atlas.width >> mip);
    initData[mip].pSysMem = atlas.bc1Data.data() + atlas.mipOffsets[mip];
    initData[mip].SysMemPitch = std::max(1u, mipW / 4) * 8;  // BC1: 8 bytes per 4x4 block row
    initData[mip].SysMemSlicePitch = 0;
}

m_device->CreateTexture2D(&td, initData.data(), &tex);
```

**Note**: `SysMemPitch` for BC1 = `max(1, width/4) * 8` (each row of 4x4 blocks is 8 bytes wide per block).

### 1.3: Modify LoadResult to carry CompressedAtlas

**File**: `terrain_renderer.h`

**Current `LoadResult`** contains:
```cpp
std::vector<uint8_t> compositeAtlas;  // 4 MB BGRA
bool hasTexture = false;
```

**Change to**:
```cpp
CompressedAtlas compressedAtlas;      // ~0.7 MB BC1 with mips
bool hasTexture = false;
```

### 1.4: Modify WorkerLoop to compress after compositing

**File**: `terrain_renderer.cpp`, `WorkerLoop()` (line 84)

After `CompositeTileAtlas()` returns the BGRA buffer, add:
```cpp
auto bgra = CompositeTileAtlas(adtData, mphdFlags, mpq);
if (!bgra.empty()) {
    result.compressedAtlas = CompressToBC1WithMips(bgra.data(), 1024, 1024);
    result.hasTexture = true;
}
```

The raw BGRA vector is released after compression (goes out of scope).

### 1.5: Verify shader compatibility

**File**: `terrain_texture_pipeline.cpp`

The pixel shader samples via `texAtlas.Sample(samLinear, i.uv)`. BC1 textures are transparently decompressed by the GPU — **no shader changes needed**. The sampler `samLinear` with `D3D11_FILTER_MIN_MAG_MIP_LINEAR` will also use mipmaps automatically (currently `MipLODBias = 0`, `MaxLOD = D3D11_FLOAT32_MAX`).

**Verify**: the current sampler desc in `TerrainTexturePipeline::Initialize()` should already have `MaxLOD = D3D11_FLOAT32_MAX`. If `MaxLOD = 0`, change it to enable mip sampling.

### Phase 1 Deliverables

- [x] `src/data/bc1_compressor.h/.cpp` — `CompressToBC1WithMips()` using stb_dxt
- [x] `terrain_renderer.h` — `LoadResult` uses `CompressedAtlas` instead of `vector<uint8_t>`
- [x] `terrain_renderer.cpp` — `WorkerLoop()` calls `CompressToBC1WithMips` after compositing
- [x] `terrain_renderer.cpp` — `UploadToGpu()` creates `BC1_UNORM` texture with N mip levels
- [x] `terrain_texture_pipeline.cpp` — verify sampler `MaxLOD` allows mipmaps
- [x] Validation: visually compare BC1 output vs BGRA (expect minor 4x4 block artifacts, acceptable for terrain)

### Expected Impact

| Metric | Before | After | Improvement |
|--------|--------|-------|-------------|
| VRAM per tile (texture) | 4.0 MB | ~0.7 MB | **5.7x** |
| VRAM total (150 tiles) | 600 MB | ~105 MB | **5.7x** |
| GPU texture bandwidth | 4 bytes/texel | 0.5 bytes/texel | **8x** |
| Distant tile quality | Aliased (no mips) | Smooth (mip-filtered) | Visual improvement |
| Tile load time (worker) | 100-200ms | 105-215ms (+5-15ms) | Negligible cost |

---

## Phase 2: Texture Array Batching

### Goal

Replace per-tile `PSSetShaderResources` bind with a single `Texture2DArray` containing all cached tile textures. Eliminates per-tile SRV state changes and enables future multi-draw optimizations.

### 2.1: Create Texture2DArray

**File**: `terrain_renderer.h` / `terrain_renderer.cpp`

Instead of individual `ID3D11Texture2D*` per tile, maintain a single large `Texture2DArray`:

```cpp
struct TextureAtlasArray {
    ComPtr<ID3D11Texture2D>          texture;     // Texture2DArray
    ComPtr<ID3D11ShaderResourceView> srv;         // SRV for entire array
    uint32_t                         capacity;    // Max slices (e.g., 256)
    uint32_t                         usedSlices;  // Currently allocated
    std::vector<int>                 freeSlots;   // Recycled slot indices
};
```

**Creation**:
```cpp
D3D11_TEXTURE2D_DESC td = {};
td.Width = 1024;
td.Height = 1024;
td.MipLevels = 10;         // BC1 mips
td.ArraySize = 256;        // Max slots
td.Format = DXGI_FORMAT_BC1_UNORM;
td.SampleDesc.Count = 1;
td.Usage = D3D11_USAGE_DEFAULT;                    // <-- DEFAULT, not IMMUTABLE
td.BindFlags = D3D11_BIND_SHADER_RESOURCE;

m_device->CreateTexture2D(&td, nullptr, &texArray);  // Empty at creation
```

**Per-tile upload** via `UpdateSubresource`:
```cpp
uint32_t slotIndex = AllocateSlot();
for (uint32_t mip = 0; mip < atlas.mipCount; ++mip) {
    uint32_t subresource = D3D11CalcSubresource(mip, slotIndex, atlas.mipCount);
    uint32_t mipW = std::max(1u, 1024u >> mip);
    uint32_t rowPitch = std::max(1u, mipW / 4) * 8;
    ctx->UpdateSubresource(texArray, subresource, nullptr,
                           atlas.bc1Data.data() + atlas.mipOffsets[mip],
                           rowPitch, 0);
}
tile.textureSlot = slotIndex;
```

### 2.2: Modify Shader to Index Array

**File**: `terrain_texture_pipeline.cpp`

**Current PS**:
```hlsl
Texture2D texAtlas : register(t0);
...
color = texAtlas.Sample(samLinear, i.uv).rgb;
```

**New PS**:
```hlsl
Texture2DArray texAtlas : register(t0);
...
color = texAtlas.Sample(samLinear, float3(i.uv, heightParams.w)).rgb;
```

Reuse `heightParams.w` (currently used as smooth normal flag, can be repurposed or add new CB field) to pass the array slice index. Alternative: add a new field to `TerrainCB`.

**Better approach**: add `uint tileSlot` to the constant buffer:
```cpp
struct TerrainCB {
    float viewProj[16];
    float lightDir[4];
    float baseColor[4];
    float heightParams[4];   // x=minZ, y=maxZ, z=colorMode, w=smoothFlag
    float tileParams[4];     // x=tileSlot, y/z/w=reserved   <-- NEW
};
```

### 2.3: Bind Once Per Frame

**File**: `terrain_renderer.cpp`, `Render()` (line 473)

**Current** (inside tile loop):
```cpp
ID3D11ShaderResourceView* srv = tileTextured ? tile.srv : nullptr;
ctx->PSSetShaderResources(0, 1, &srv);
```

**New** (before tile loop):
```cpp
ctx->PSSetShaderResources(0, 1, &m_texArray.srv);  // Bind once
```

Inside tile loop, only update CB with `tileParams.x = tile.textureSlot`:
```cpp
cb.tileParams[0] = static_cast<float>(tile.textureSlot);
```

### 2.4: Slot Management

When tile is evicted from cache, return its slot to `freeSlots`. When a new tile is uploaded, pop from `freeSlots` or increment `usedSlices`.

### Phase 2 Deliverables

- [x] `terrain_renderer.h` — `TextureAtlasArray` struct, slot allocator
- [x] `terrain_renderer.cpp` — Create `Texture2DArray` (256 slots, BC1, 10 mips)
- [x] `terrain_renderer.cpp` — `UploadToGpu()` uses `UpdateSubresource` per slice
- [x] `terrain_renderer.cpp` — `Render()` binds SRV once, passes slot via CB
- [x] `terrain_texture_pipeline.cpp` — PS uses `Texture2DArray` + `float3(uv, slot)`
- [x] `terrain_renderer.cpp` — Evict returns slot to `freeSlots`
- [x] Validation: all textured tiles render correctly, no slot leak on pan/zoom

### Expected Impact

| Metric | Before | After | Improvement |
|--------|--------|-------|-------------|
| SRV binds per frame | 10-20 | 1 | **10-20x fewer state changes** |
| Total state changes per tile | 3 (CB + SRV + Draw) | 2 (CB + Draw) | **~1.5x** |
| VRAM layout | Scattered textures | Single contiguous array | Better cache locality |

**Note**: `D3D11_USAGE_DEFAULT` + `UpdateSubresource` replaces `IMMUTABLE` + `CreateTexture2D`. This allows incremental slot updates without recreating the entire array.

---

## Phase 3: 3D Overlay Frustum Culling

### Goal

Add camera frustum culling to `Graph3DRenderer` and other 3D overlay renderers to skip geometry generation for off-screen nodes/edges. Currently ALL nodes and edges are processed every frame regardless of visibility.

### 3.1: Add Frustum Test to Graph3DRenderer::Render

**File**: `graph_renderer_3d.cpp`, `Render()` (line 51)

**Current edge loop** (no culling):
```cpp
for (size_t i = 0; i < graph.GetEdges().size(); ++i) {
    const auto& edge = graph.GetEdges()[i];
    const auto* from = graph.GetNode(edge.fromNode);
    const auto* to   = graph.GetNode(edge.toNode);
    if (!from || !to) continue;
    if (from->mapId != mapId && to->mapId != mapId) continue;
    // ... AddLine
}
```

**Add AABB frustum test** (reuse `Camera3D::GetFrustumPlanes`):

```cpp
void Graph3DRenderer::Render(const Camera3D& camera, ...) {
    float frustum[6][4];
    camera.GetFrustumPlanes(frustum);

    // Edges: cull if both endpoints outside frustum
    for (size_t i = 0; i < graph.GetEdges().size(); ++i) {
        const auto* from = graph.GetNode(edge.fromNode);
        const auto* to   = graph.GetNode(edge.toNode);
        if (!from || !to) continue;
        if (from->mapId != mapId && to->mapId != mapId) continue;

        // Quick sphere test: skip if BOTH endpoints outside any frustum plane
        if (!PointInFrustum(frustum, from->x, from->y, from->z) &&
            !PointInFrustum(frustum, to->x, to->y, to->z))
            continue;

        prims.AddLine(...);
    }

    // Nodes: cull individual nodes
    for (const auto& node : graph.GetNodes()) {
        if (node.mapId != mapId) continue;
        if (!PointInFrustum(frustum, node.x, node.y, node.z))
            continue;
        prims.AddCircle(...);
    }
}
```

**`PointInFrustum` helper** (simple point-vs-6-planes):
```cpp
static bool PointInFrustum(const float planes[6][4], float x, float y, float z) {
    for (int i = 0; i < 6; ++i) {
        float d = planes[i][0] * x + planes[i][1] * y + planes[i][2] * z + planes[i][3];
        if (d < -50.0f) return false;  // 50-yard margin for large node circles
    }
    return true;
}
```

The 50-yard margin prevents popping artifacts for nodes near screen edges.

### 3.2: Same for RenderIssueOverlay

**File**: `graph_renderer_3d.cpp`, `RenderIssueOverlay()` (line 155)

Same pattern: compute frustum planes, skip off-screen edges/nodes/gaps.

### 3.3: Same for 2D edge culling

**File**: `graph_renderer.cpp`, `Render()` (line 60)

Currently edges have no frustum test. Add viewport bounds check before `WorldToScreen`:

```cpp
// Before edge loop, already have:
float minX, maxX, minY, maxY;
canvas.GetViewBounds(minX, maxX, minY, maxY);

// In edge loop, add:
if (from->x < minX && to->x < minX) continue;
if (from->x > maxX && to->x > maxX) continue;
if (from->y < minY && to->y < minY) continue;
if (from->y > maxY && to->y > maxY) continue;
```

This is a simple AABB rejection that skips `WorldToScreen` computation for fully off-screen edges.

### Phase 3 Deliverables

- [x] `graph_renderer_3d.cpp` — `Render()` uses frustum culling for nodes and edges
- [x] `graph_renderer_3d.cpp` — `RenderIssueOverlay()` uses frustum culling
- [x] `graph_renderer.cpp` — `Render()` adds viewport AABB rejection for edges
- [x] `route_renderer_3d.cpp` — frustum cull waypoints (optional, low impact)
- [x] Validation: verify no visual popping, especially for nodes near screen edges

### Expected Impact

| Metric | Before | After | Improvement |
|--------|--------|-------|-------------|
| Nodes processed (3D) | ALL | ~20-40% visible | **2.5-5x fewer** |
| Edges processed (3D) | ALL | ~20-40% visible | **2.5-5x fewer** |
| Vertices generated (3D) | 35K typical | 7-14K typical | **2.5-5x fewer** |
| CPU time (overlay gen) | ~0.3-0.5ms | ~0.1-0.2ms | **2-3x** |
| 2D edge WorldToScreen | ALL edges | ~30-50% visible | **2-3x fewer** |

---

## Phase 4: Graph Geometry Caching (Dirty Flag + Static VB)

### Goal

Stop rebuilding the entire graph vertex buffer every frame. Instead, build a static `ID3D11Buffer` when the graph changes and reuse it across frames. The graph changes only when the user adds/removes/moves nodes/edges or changes selection — which is rare compared to 60 fps rendering.

### 4.1: GraphMeshCache

**New file**: `src/render/graph_mesh_cache.h` / `.cpp`

```cpp
namespace mapedit {

class GraphMeshCache {
public:
    void Initialize(ID3D11Device* device);
    void Shutdown();

    // Call when graph data or selection changes
    void Invalidate();

    // Rebuild if dirty. Returns true if cache is valid for rendering.
    // frustum: 6 planes for culling (optional, can build full mesh and let GPU clip)
    bool Update(ID3D11Device* device,
                const WorldGraphData& graph,
                uint32_t mapId,
                const MultiSelection& selection,
                const LayerVisibility& layers,
                float dimAlpha);

    // Bind and draw cached geometry
    void Render(ID3D11DeviceContext* ctx, const float viewProj[16]);

    uint32_t GetVertexCount() const { return m_vertexCount; }

private:
    ComPtr<ID3D11Buffer>  m_vb;           // Static vertex buffer (LineVertex[])
    ComPtr<ID3D11Buffer>  m_cb;           // VP constant buffer
    uint32_t              m_vertexCount = 0;
    bool                  m_dirty = true;
    uint64_t              m_graphVersion = 0;   // Track graph modification counter
    size_t                m_selectionHash = 0;  // Track selection changes
};

} // namespace mapedit
```

### 4.2: Graph Version Tracking

**File**: `src/data/world_graph_data.h`

Add a modification counter to `WorldGraphData`:

```cpp
class WorldGraphData {
public:
    // ... existing API ...
    uint64_t GetVersion() const { return m_version; }

private:
    uint64_t m_version = 0;  // Incremented on every AddNode/RemoveNode/AddEdge/RemoveEdge/MoveNode
};
```

Every mutating operation increments `m_version`. The cache compares its stored version to detect changes.

### 4.3: Selection Hash

**File**: `src/editor/selection.h`

Add a hash to `MultiSelection`:

```cpp
class MultiSelection {
public:
    size_t GetHash() const { return m_hash; }
private:
    size_t m_hash = 0;  // Updated on select/deselect
};
```

### 4.4: Integration

**File**: `src/render/graph_renderer_3d.cpp`

Replace per-frame geometry generation with cache check:

```cpp
void Graph3DRenderer::Render(const Camera3D& camera, ...) {
    // Check if cache needs rebuild
    if (m_cache.Update(m_device, graph, mapId, selection, layers, dimAlpha)) {
        // Cache valid — just render
        m_cache.Render(m_context, camera.GetViewProj());
        return;
    }
    // Fallback: generate via Primitives3D (should not happen in steady state)
    // ... existing code ...
}
```

**Important**: The cached VB uses the same `LineVertex` format and same shaders as `Primitives3D`. Can reuse `Primitives3D` pipeline state (VS, PS, blend, depth, rasterizer) — just bind a different VB.

### 4.5: What Invalidates the Cache

| Event | Detection | Source |
|-------|-----------|--------|
| Node added/removed/moved | `graph.GetVersion()` changed | `WorldGraphData` |
| Edge added/removed | `graph.GetVersion()` changed | `WorldGraphData` |
| Selection changed | `selection.GetHash()` changed | `MultiSelection` |
| Map changed | `mapId` parameter changed | App |
| Layer toggles (labels, etc.) | `layers` struct changed | LayerVisibility |
| dimAlpha changed | Direct comparison | Render params |

### Phase 4 Deliverables

- [x] `src/data/world_graph_data.h` — `m_version` counter, `GetVersion()`
- [x] `src/editor/selection.h` — `m_hash`, `GetHash()`
- [x] `src/render/graph_mesh_cache.h/.cpp` — Cache with dirty flag + static VB
- [x] `src/render/graph_renderer_3d.cpp` — Use cache in `Render()`
- [x] Validation: editing operations (add node, move, select) instantly update display

### Expected Impact

| Metric | Before | After | Improvement |
|--------|--------|-------|-------------|
| CPU work (steady state) | Full rebuild every frame | Version check only | **50-100x** (microseconds vs ms) |
| VB upload | Map/Unmap WRITE_DISCARD 60x/sec | Only on graph edit | **~0 uploads/sec steady** |
| Memory coherence | Rebuild vector + memcpy | Static GPU buffer | Better cache behavior |

**Note**: Other overlays (routes, paths, player marker) still use `Primitives3D` per-frame — their vertex counts are tiny (<1K) and don't benefit from caching.

---

## Phase 5: Node Instancing + CB Batching

### Goal

Two smaller optimizations that further reduce GPU overhead:

1. **Node instancing**: Render circles as instanced geometry instead of N×16 line segments
2. **CB batching**: Reduce per-tile constant buffer updates for terrain

### 5.1: Instanced Node Circles

**Concept**: Create a unit circle mesh once (16 segments, 32 vertices as LINELIST). For each visible node, add an instance with per-instance data (position, radius, color). One `DrawInstanced` call renders all nodes.

**New instance buffer layout**:
```cpp
struct NodeInstance {
    float x, y, z;       // World position
    float radius;         // Circle radius
    uint32_t color;       // ABGR
};  // 20 bytes per instance
```

**Vertex shader modification**:
```hlsl
struct VS_IN {
    float2 circlePos : POSITION;    // Unit circle vertex (per-vertex)
    float3 center    : INST_POS;    // Node center (per-instance)
    float  radius    : INST_RAD;    // Node radius (per-instance)
    float4 color     : INST_COL;    // Node color (per-instance)
};
VS_OUT main(VS_IN i) {
    float3 worldPos = i.center + float3(i.circlePos * i.radius, 0);
    o.pos = mul(float4(worldPos, 1.0), viewProj);
    o.col = i.color;
    return o;
}
```

**Draw call**:
```cpp
ctx->DrawInstanced(32, visibleNodeCount, 0, 0);  // 1 call for ALL nodes
```

**Impact**: 1000 nodes = 1 draw call (vs 1000 × AddCircle = 32K individual vertices).

### 5.2: Terrain CB Batching

**Concept**: Instead of `Map/Unmap` the constant buffer per visible tile, use a structured buffer with per-tile parameters and index by `SV_InstanceID` or a push constant.

**Simpler alternative**: Since only `heightParams.z` (colorMode) and `tileParams.x` (texture slot) change per tile, and most tiles have the same colorMode (3.0 = textured), batch tiles by colorMode:

```cpp
// Group tiles: textured vs procedural
std::vector<TileGpu*> texturedTiles, proceduralTiles;
for (auto& [key, tile] : m_gpuCache) {
    if (tile.hasTexture) texturedTiles.push_back(&tile);
    else proceduralTiles.push_back(&tile);
}

// One CB update for textured batch
cb.heightParams[2] = 3.0f;
UpdateCB(cb);
for (auto* tile : texturedTiles) {
    // Only update tileParams (slot index)
    // ... DrawIndexed
}

// One CB update for procedural batch
cb.heightParams[2] = static_cast<float>(colorMode);
UpdateCB(cb);
for (auto* tile : proceduralTiles) {
    ctx->DrawIndexed(tile->indexCount, 0, 0);
}
```

This reduces CB updates from N to 2 (or 1 if all tiles are textured).

### Phase 5 Deliverables

- [x] `primitives_3d.h/.cpp` — Add `AddCircleInstanced()` path + instance buffer
- [x] `primitives_3d.cpp` — New VS with per-instance transform
- [x] `graph_renderer_3d.cpp` — Use instanced path for node rendering
- [x] `terrain_renderer.cpp` — Batch tiles by colorMode, reduce CB updates
- [x] Validation: instanced circles match current visual appearance

### Expected Impact

| Metric | Before | After | Improvement |
|--------|--------|-------|-------------|
| Node vertices (1000 nodes) | 32,000 | 32 (instanced) | **1000x fewer vertices** |
| Node draw calls | 1 (already batched) | 1 (instanced) | Same count, less data |
| CB updates (terrain) | 10-20 per frame | 1-2 per frame | **5-10x fewer** |

---

## Implementation Order & Dependencies

```
Phase 1 (BC1 + Mips)          -- No dependencies, standalone
    ↓
Phase 2 (Texture Array)       -- Depends on Phase 1 (BC1 format assumption)

Phase 3 (Frustum Culling)     -- No dependencies, standalone
    ↓
Phase 4 (Graph Caching)       -- Independent, but benefits from Phase 3 (cached mesh can skip culling)

Phase 5 (Instancing + CB)     -- Depends on Phase 2 (CB batching uses texture array) + Phase 4 (instancing integrates with cache)
```

**Recommended execution**:
- Phase 1 and Phase 3 can be done **in parallel** (no shared files)
- Phase 2 after Phase 1
- Phase 4 after Phase 3 (or in parallel if different developer)
- Phase 5 last (depends on both tracks)

---

## Risk Assessment

| Risk | Likelihood | Mitigation |
|------|------------|------------|
| BC1 visual quality loss | Low | 4x4 blocks → minor banding on sharp edges. Acceptable for terrain. Test on Stormwind cobblestones |
| Texture Array VRAM spike | Medium | 256 slots × 0.7 MB = 180 MB allocated upfront. If too much, use smaller capacity (128 slots) |
| Graph cache invalidation missed | Medium | Add `Invalidate()` calls at ALL graph mutation points. Fallback: force-invalidate every 5 seconds as safety net |
| Instanced circles vs line circles | Low | Visual difference: instanced uses fixed orientation (XY plane). For 3D, circles should face camera (billboard). May need billboard transform in VS |
| `D3D11_USAGE_DEFAULT` + `UpdateSubresource` stalls | Low | `UpdateSubresource` on DEFAULT textures may stall if GPU is reading. Mitigate: upload during `UpdateViewport()` (before render loop) |

---

## Validation Checklist

- [x] BC1 textures visually match BGRA (side-by-side comparison at Goldshire/Stormwind)
- [x] Mipmaps eliminate distant tile aliasing (compare zoom-out before/after)
- [x] Texture Array renders all tiles correctly (no slot confusion, no black tiles)
- [x] Frustum culling: no node/edge popping when panning (50-yard margin)
- [x] Graph cache: editing operations update display immediately
- [x] Graph cache: steady-state CPU overlay time < 0.1ms (profiler check)
- [x] VRAM usage reported in status bar or profiler (before/after comparison)
- [x] No regressions in 2D mode rendering
- [x] Frame profiler shows reduced Terrain and Overlay layer times
