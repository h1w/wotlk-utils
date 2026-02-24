# Performance Profiler Overlay & FPS Limiter

**Date:** 2026-02-24

## Summary

Added a real-time performance profiler overlay to 3D mode with per-layer CPU timing, draw call/vertex counts, VSync toggle, and an FPS limiter slider. Disabled VSync by default to remove the vsync staircase effect that was masking true performance.

## Problem

The map editor had no way to measure rendering performance. VSync was hardcoded ON (`Present(1, 0)`), which caused a "staircase" effect: if a frame took 17ms (0.3ms over the 16.7ms budget), the next present would wait for the next vsync boundary = 33ms total (30 FPS). In Debug builds, this combined with debug device overhead and disabled optimizations resulted in ~10 FPS despite powerful hardware being available.

There was also no way to limit FPS to a target value (e.g., 144) without VSync.

## Solution

### Frame Profiler

Created `FrameProfiler` — a lightweight header-only profiler using `QueryPerformanceCounter` for sub-microsecond timing. It measures CPU time for each rendering layer (Terrain, Buildings, Navmesh, Overlays), averages over 60 frames for stable display, and collects draw call + vertex counts from each renderer.

### Per-Renderer Stats

Added `statDrawCalls` and `statVertices` counters to `TerrainRenderer`, `BuildingRenderer`, and `NavmeshRenderer3D`. Each renderer resets and accumulates these during its `Render()` call.

### FPS Limiter

Spin-wait loop using `QueryPerformanceCounter` with `_mm_pause()` CPU hint. Provides sub-millisecond accuracy without relying on Windows timer resolution (which has 15.6ms granularity, making `Sleep(1)` unusable for frame pacing).

### ImGui Overlay

The "Performance" window displays:
- FPS and frame time (averaged)
- Per-layer breakdown: progress bar + milliseconds + draw calls + vertex count
- Total draw calls and vertices
- VSync checkbox
- FPS Limit slider (0-300, appears when VSync is off)

## New Files

| File | Description |
|------|-------------|
| `src/render/frame_profiler.h` | Header-only `FrameProfiler` struct with QPC timing, per-layer accumulation, 60-frame averaging |

## Modified Files

| File | Change |
|------|--------|
| `src/app.h` | Added `FrameProfiler`, `m_vsync`, `m_showProfiler`, `m_fpsLimit`, `m_frameStartQpc` members |
| `src/app.cpp` | Profiler init, per-layer timing in `RenderFrame3D()`, profiler ImGui overlay, FPS limiter spin-wait before `Present()`, VSync now toggleable (`Present(m_vsync ? 1 : 0, 0)`) |
| `src/render/terrain_renderer.h` | Added `statDrawCalls`, `statVertices` public members |
| `src/render/terrain_renderer.cpp` | Count draw calls and vertices in `Render()` loop |
| `src/render/building_renderer.h` | Added `statDrawCalls`, `statVertices` public members |
| `src/render/building_renderer.cpp` | Count draw calls and vertices in all three draw paths (no-group, portal-off, portal-culled) |
| `src/render/navmesh_renderer_3d.h` | Added `statDrawCalls`, `statVertices` public members |
| `src/render/navmesh_renderer_3d.cpp` | Count draw calls and vertices in `Render()` loop |

## Technical Details

### VSync Staircase

With `Present(1, 0)`, frame times snap to multiples of the vsync interval (16.7ms at 60Hz). A frame taking 17ms doesn't result in 58 FPS — it results in 30 FPS because the driver waits for the next vsync boundary. This was the primary reason for perceived low FPS in Release builds.

### FPS Limiter Implementation

```
Sleep(1) on Windows = 15.6ms (default timer granularity)
   -> 1000/15.6 = 64 FPS max
   -> Two sleeps = 31.2ms = 32 FPS

Spin-wait with _mm_pause() = sub-microsecond accuracy
   -> Exact target FPS regardless of OS timer resolution
   -> _mm_pause() reduces CPU power consumption during spin
```

### Debug vs Release

The investigation revealed the user was running a **Debug build**, which explained the 10 FPS:
- `/Od` (no optimization) = 3-5x slower code
- `D3D11_CREATE_DEVICE_DEBUG` = 10-50x overhead on GPU commands
- `_ITERATOR_DEBUG_LEVEL=2` = 2-5x slower STL container iteration

Release build immediately achieved 400-600+ FPS on the same hardware (RTX 4070 Super).
