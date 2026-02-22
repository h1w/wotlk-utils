# GPU-accelerated navmesh rendering

**Date:** 2026-02-22

## Summary

Replaced ImGui immediate-mode triangle rendering with a native DX11 GPU pipeline. Per-tile immutable vertex buffers, vertex shader world-to-NDC transform, pixel shader barycentric wireframe, AABB viewport culling, LOD (base polygons vs detail mesh). Added configurable tile cache size slider.

## Problem

The navmesh renderer used `ImDrawList::AddTriangleFilled` + `AddTriangle` per triangle (~800K calls at 200 tiles). This was CPU-bound: ~2.4M vertex transforms per frame, ImGui vertex buffer splits at 64K, excessive draw calls. FPS dropped to 5-10 when zoomed out with many tiles visible.

## Solution

### Native DX11 pipeline (`NavmeshPipeline`)

- Embedded HLSL compiled at startup via `D3DCompile`
- **Vertex shader**: 2 subtracts + 2 multiplies + 1 negate per vertex, on GPU. Transforms world coords (wowX, wowY) directly to NDC using canvas center/zoom
- **Pixel shader**: barycentric wireframe in single pass via `fwidth`/`smoothstep`. When edges hidden, `edgeColor = fillColor` eliminates wireframe with zero branching
- **State objects**: alpha blend, no cull, scissor enabled, depth disabled
- **Constant buffer** (48 bytes): `{float4 transform, float4 fillColor, float4 edgeColor}`
- **Vertex format**: `{float2 pos, float3 bary}` = 20 bytes per vertex, unrolled (no index buffer)

### GPU tile cache (`NavmeshRenderer`)

- `std::map<TileKey, TileGpuData>` synced with `TileCache` each frame (lazy create/release)
- Each `TileGpuData` holds two `ID3D11Buffer*` (IMMUTABLE vertex buffers):
  - **detailVB**: full detail mesh triangles (from `ExtractTriangles`)
  - **baseVB**: fan-triangulated base polygons (from `ExtractBasePolygons`), 80-90% fewer triangles
- Per-tile world-space AABB computed at upload time
- GPU cache flushed on map change

### Render flow

1. Sync GPU cache with TileCache (create new VBs, release evicted ones; throttled: max 8 uploads/frame)
2. LOD selection: detail mesh when `zoom >= 0.1`, base polygons otherwise
3. `ImGui::GetBackgroundDrawList()->AddCallback(DrawCallback, &data)` injects DX11 draw calls at correct z-order
4. `AddCallback(ImDrawCallback_ResetRenderState, nullptr)` restores ImGui pipeline state

Note: viewport culling is handled at the TileCache level (only viewport-relevant tiles are loaded), so the GPU cache inherits this naturally via SyncGpuCache.

### DrawCallback internals

1. Set DX11 viewport + scissor to canvas area
2. Bind VS, PS, input layout, blend, rasterizer, depth-stencil
3. Map/Unmap constant buffer with current canvas transform + colors
4. For each visible tile: `IASetVertexBuffers` + `Draw(vertCount, 0)`

### Base polygon extraction (`ExtractBasePolygons`)

New function in `tile_loader`. Iterates `dtPoly` (not `dtPolyDetail`), fan-triangulates each convex polygon: for N-vertex poly, produces N-2 triangles using `tile->verts[poly.verts[i]]`. Same `TileTriangles` output format as `ExtractTriangles`.

### Configurable tile cache size

- `TileCache::kMaxCachedTiles` replaced with runtime `m_maxTiles` (setter/getter)
- "Max Tiles" slider in Layers panel (range 50-600, default 200)
- Persisted in `map_editor_settings.json` as `layers.navmesh_max_tiles`

## NDC math derivation

Canvas `WorldToScreen`:
```
sx = vpCenterX + (centerY - wowY) * zoom
sy = vpCenterY + (centerX - wowX) * zoom
```

With DX11 viewport set to canvas area `(vpX, vpY, vpW, vpH)`:
```
NDC_x = (centerY - wowY) * (2 * zoom / vpW)
NDC_y = -(centerX - wowX) * (2 * zoom / vpH)
```

Vertex shader:
```hlsl
o.pos.x =  (transform.y - i.pos.y) * transform.z;   // transform.y=centerY, .z=scaleX
o.pos.y = -(transform.x - i.pos.x) * transform.w;   // transform.x=centerX, .w=scaleY
```

## Memory budget

| Resource | Per tile | 200 tiles |
|----------|----------|-----------|
| Detail VB (~4000 tri * 3 * 20 B) | ~240 KB | ~48 MB |
| Base VB (~500 tri * 3 * 20 B) | ~30 KB | ~6 MB |
| **Total GPU** | ~270 KB | **~54 MB** |

## Files changed

| File | Change |
|------|--------|
| `src/render/navmesh_pipeline.h` | **New.** GPU pipeline class: shaders, state objects, constant buffer |
| `src/render/navmesh_pipeline.cpp` | **New.** Embedded HLSL, D3DCompile, DX11 resource creation |
| `src/render/navmesh_renderer.h` | **Rewritten.** GPU tile cache, AddCallback integration, TileGpuData/CallbackData structs |
| `src/render/navmesh_renderer.cpp` | **Rewritten.** SyncGpuCache, AABB culling, LOD, DrawCallback with full DX11 pipeline |
| `src/navmesh/tile_loader.h` | Added `ExtractBasePolygons` declaration |
| `src/navmesh/tile_loader.cpp` | Added `ExtractBasePolygons` implementation (fan-triangulated dtPoly) |
| `src/navmesh/tile_cache.h` | `kMaxCachedTiles` → `kDefaultMaxTiles` + runtime `m_maxTiles` with setter/getter |
| `src/navmesh/tile_cache.cpp` | Use `m_maxTiles` instead of constexpr |
| `src/render/graph_renderer.h` | Added `navmeshMaxTiles` to `LayerVisibility` |
| `src/ui/layer_panel.cpp` | Added "Max Tiles" slider under Navmesh section |
| `src/data/app_settings.h` | Added `navmeshMaxTiles` field |
| `src/data/app_settings.cpp` | Load/save `navmesh_max_tiles` in layers JSON |
| `src/app.cpp` | `NavmeshRenderer::Initialize`/`Shutdown`, `SetMaxTiles` before `UpdateViewport`, persist `navmeshMaxTiles` |
| `map-editor.vcxproj` | Added `navmesh_pipeline.h`/`.cpp` to build |
