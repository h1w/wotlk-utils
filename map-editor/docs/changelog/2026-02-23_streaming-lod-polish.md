# Background loading, terrain LOD, and building culling

**Date:** 2026-02-23
**Task:** TASK-003, Phase 5

## Summary

Moved terrain and building tile loading to background worker threads, added terrain LOD via mesh decimation, and implemented small-object culling for M2 building models. These changes eliminate frame hitches during tile loading and reduce GPU workload for distant tiles.

## Modified Files

| File | Change |
|------|--------|
| `src/data/terrain_mesh.h` | Added `decimation` parameter (default 1) to `GenerateTerrainMesh` |
| `src/data/terrain_mesh.cpp` | Rewrote mesh generation with LOD decimation support (factors 1/2/4/8) — V9 subsampling, V8 averaging, conservative hole check |
| `src/render/terrain_renderer.h` | Added background loading infrastructure: `LoadRequest`/`LoadResult` structs, worker thread, request/result queues with mutexes, pending tile set, `SelectLOD()` |
| `src/render/terrain_renderer.cpp` | Full rewrite: worker thread owns its own `TerrainLoader`, LOD selection by distance, throttled GPU uploads (4/frame), map-change queue flush, empty sentinel tiles |
| `src/render/building_renderer.h` | Added background loading infrastructure: `LoadRequest`/`LoadResult` structs, worker thread, request/result queues with mutexes, pending tile set |
| `src/render/building_renderer.cpp` | Full rewrite: worker thread owns its own `BuildingLoader`+`VMapTileLoader`, small-object culling for M2 models, `TransformVertices` moved to static free function |

## Technical Details

### Background Loading (Terrain + Buildings)

Both renderers now follow the same pattern (modeled after `MinimapTileCache`):

```
Main thread                            Worker thread
───────────                            ─────────────
UpdateViewport()                       WorkerLoop()
  ├─ dequeue LoadResult → GPU upload     ├─ wait on condition_variable
  ├─ evict distant tiles (LRU)           ├─ dequeue LoadRequest
  └─ queue LoadRequest for new tiles     ├─ load from disk (CPU-heavy)
                                         └─ post LoadResult
```

- Each worker owns its own loader instances (no shared mutable state)
- Data path passed per-request (thread-safe without sharing)
- Map change: main thread clears request queue, result queue, GPU cache, and pending set; worker detects via `mapId` mismatch and discards stale results
- Empty sentinel entries (`TileGpu{}`/`TileBuildings{}`) inserted for tiles with no data file to prevent re-queuing

**Terrain throttling**: max 4 GPU uploads/frame, max 8 tile requests/frame, 150-tile cache cap
**Building throttling**: max 2 GPU uploads/frame, max 4 tile requests/frame, 100-tile cache cap

### Terrain LOD (Decimation)

`GenerateTerrainMesh` accepts a `decimation` factor (1, 2, 4, or 8):

| Decimation | Cells/side | V9 vertices | V8 vertices | Max triangles | Reduction |
|-----------|------------|-------------|-------------|---------------|-----------|
| 1 (full)  | 128        | 16,641      | 16,384      | 65,536        | 1x        |
| 2 (half)  | 64         | 4,225       | 4,096       | 16,384        | 4x        |
| 4 (quarter)| 32        | 1,089       | 1,024       | 4,096         | 16x       |
| 8 (eighth)| 16         | 289         | 256         | 1,024         | 64x       |

- **V9 corners**: subsampled at stride `D` from the 129x129 grid
- **V8 centers**: averaged over the DxD block of original V8 values (smooth height transitions)
- **Holes**: conservative check — any hole in the DxD source block skips the entire decimated cell
- **LOD selection** (`SelectLOD`): tile distance < 2 → decimation 1, < 5 → 2, else → 4

### Building Small-Object Culling

In the worker thread, M2 models (flag `MOD_M2 = 0x01`) are skipped when:
- Camera distance > 500 yards (`kSmallObjectCullDist`)
- Model bounding box max dimension × spawn scale < 3 yards (`kSmallObjectSize`)

This filters out small props (barrels, crates, signs, fences) that contribute minimal visual information at distance, reducing vertex count and GPU draw time for distant building tiles.

### Thread Safety

| Shared state | Protection |
|---|---|
| Request queue (`m_requests`) | `m_reqMutex` + `m_reqCV` |
| Result queue (`m_results`) | `m_resMutex` |
| Data path (`m_dataPath`) | `m_pathMutex` (read per-request into `LoadRequest::dataPath`) |
| GPU cache (`m_gpuCache`) | Main thread only (never accessed by worker) |
| Pending set (`m_pending`) | Main thread only |
| Loader instances | Worker-private (no sharing) |
