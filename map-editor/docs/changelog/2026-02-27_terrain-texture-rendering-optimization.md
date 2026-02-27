# Terrain Texture Rendering Optimization

**Date:** 2026-02-27

## Summary

Three-stage optimization eliminating per-tile constant buffer updates, fixing mutex contention that caused <30 FPS at medium zoom, and parallelizing texture loading across multiple worker threads. Terrain texture rendering now has negligible per-frame cost.

## Stage 1: Per-Vertex Texture Slot Index + Anisotropic Filtering

**Goal**: Eliminate per-tile CB Map/Unmap for textured tiles. Each textured tile previously required its own `D3D11_MAP_WRITE_DISCARD` + `memcpy` to set `tileParams[0]` (texture array slot index). With 10-20 visible textured tiles, this meant 10-20 Map/Unmap cycles per frame.

**Approach**: Bake the texture array slot index into each vertex at upload time. The shader reads the slot from vertex input (`TEXCOORD1`) instead of the constant buffer. All textured tiles share a single CB update per frame.

**Changes**:

- `TerrainVertex` (CPU) and `TerrainVertexGpu` (GPU): added `float slotIndex` field (32 → 36 bytes)
- `UploadToGpu()`: allocates texture array slot FIRST, patches all vertex `slotIndex` values, THEN creates IMMUTABLE vertex buffer
- Textured tile render loop: single CB update with `heightParams[2] = 3.0f` for entire batch (matching procedural batch pattern)
- HLSL vertex shader: `float slotIdx : TEXCOORD1` input, passthrough to `TEXCOORD3`
- HLSL pixel shader: `texAtlas.Sample(samLinear, float3(i.uv, i.slotIdx))` instead of `tileParams.x`
- Input layout: added 4th element `{"TEXCOORD", 1, DXGI_FORMAT_R32_FLOAT, 0, 32, ...}`
- Sampler upgraded from bilinear (`D3D11_FILTER_MIN_MAG_MIP_LINEAR`, MaxAnisotropy=1) to anisotropic (`D3D11_FILTER_ANISOTROPIC`, MaxAnisotropy=8) — better oblique-angle quality at negligible GPU cost

## Stage 2: Mutex Contention Fix

**Problem**: After Stage 1, FPS was 200+ at close and far zoom but dropped below 30 at medium zoom.

**Root cause**: `m_texMutex` contention. The worker thread held the mutex for 200-500 ms during ADT parsing, BLP loading, texture compositing, and BC1 compression — all while the main thread's `Render()` and `UpdateViewport()` tried to lock the same mutex every frame just to read a bool.

- Close zoom: few tiles to load → worker mostly idle → no contention → 200+ FPS
- Far zoom: tiles beyond texture distance → no texture loading → 200+ FPS
- Medium zoom: many tiles actively loading textures → worker constantly holding lock → main thread blocked → <30 FPS

**Fix**: Moved `AdtTextureParser`, `BlpTextureCache`, and `TerrainTextureCompositor` from class members to worker-local variables. Worker now holds `m_texMutex` only for microseconds (snapshot MPQ pointer + config), then releases before doing any heavy work.

**Also added**: Camera-distance-aware LOD. `SelectLOD(tileDist, cameraDist)` scales distance thresholds by `max(1, cameraDist / 300)`, reducing triangle counts at medium-far zoom.

## Stage 3: Multi-Threaded Texture Loading

**Problem**: After Stage 2, FPS was smooth but textures only appeared on 6-7 center tiles and loaded very slowly when panning. Single worker thread throughput ~2-5 tiles/sec (each tile: ADT parse + BLP reads + compositing + BC1 compression ≈ 200-500 ms).

**Fix**: Changed from 1 worker thread to 3 parallel workers (`kWorkerCount = 3`). All share the same request/result queues (already mutex-protected). Added `m_mpqMutex` to serialize MPQ reads (StormLib is not thread-safe for concurrent reads from the same handle). BC1 compression runs outside all locks, enabling true parallelism.

**Pipeline**: Worker A loads mesh + composites (MPQ lock), Worker B BC1-compresses a previous tile (no lock), Worker C loads another mesh (waiting for MPQ lock) — keeps all 3 busy.

## Modified Files

| File | Changes |
|------|---------|
| `src/data/terrain_mesh.h` | `TerrainVertex`: +`float slotIndex` (32 → 36 bytes) |
| `src/render/terrain_pipeline.h` | `TerrainVertexGpu`: +`float slotIndex` (32 → 36 bytes) |
| `src/render/terrain_texture_pipeline.cpp` | VS/PS shader `slotIdx` passthrough; 4th input layout element; anisotropic 8x sampler |
| `src/render/terrain_renderer.h` | Removed compositor class members; `UploadToGpu(LoadResult&)` non-const; `SelectLOD` +cameraDist; 3 workers + `m_mpqMutex` |
| `src/render/terrain_renderer.cpp` | UploadToGpu slot-bake-before-VB; WorkerLoop worker-local compositor + brief mutex; multi-worker Start/Stop; camera-aware LOD; single CB batch for textured tiles |

## Performance Summary

| Metric | Before | After |
|--------|--------|-------|
| CB Map/Unmap per frame (textured) | N (per tile) | 1 (batch) |
| Vertex size | 32 bytes | 36 bytes (+12.5%) |
| Texture quality at angles | Bilinear | Anisotropic 8x |
| m_texMutex hold time (worker) | 200-500 ms | < 0.01 ms |
| Texture loading workers | 1 | 3 (parallel) |
| Tile load throughput | ~2-5 tiles/sec | ~6-12 tiles/sec |
| FPS at medium zoom | < 30 | 200+ |
