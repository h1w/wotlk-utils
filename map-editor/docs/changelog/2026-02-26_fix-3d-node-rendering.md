# Fix 3D Node Circle Rendering

**Date:** 2026-02-26

## Summary

Fixed road graph node circles not rendering in 3D view for most of the continent. Only nodes in the northernmost area (near camera spawn) were visible; the rest showed only edge lines. Root cause: the `Primitives3D` vertex budget (65,536) was too small for the combined geometry (grid tiles + edges + node circles ≈ 67,000+ vertices). Circles were silently dropped when the budget ran out. Also reduced node circle radius by 30% for better visual clarity.

## Root Cause

Rendering order in `Primitives3D` per frame:
1. **Grid** — ~687 tiles × 6 lines × 2 verts = **~8,244 vertices**
2. **Edge lines** — ~1,488 edges × 2 verts = **~2,976 vertices**
3. **Node circles** — ~1,752 nodes × 16 segments × 2 verts = **~56,064 vertices**
4. **Total: ~67,284 vertices > 65,536 limit**

`AddCircle()` silently returned when the budget was exhausted — no log warning. Because nodes are iterated in load order (north-to-south from tile extraction), only the first ~1,700 northern nodes fit; the remaining southern nodes were dropped.

## Changes

### Vertex Budget Increase (`primitives_3d.h`)

- `kMaxVertices`: `65536` → `524288` (512K vertices, 8 MB GPU buffer)
- Provides 7.5× headroom for future growth (routes, paths, player markers, etc.)

### Budget Exhaustion Logging (`primitives_3d.cpp`)

- `AddLine()` — logs `WARNING` on first budget overflow via `LOG_FIRST_N(WARNING, 1)`
- `AddCircle()` — logs `WARNING` on first budget overflow via `LOG_FIRST_N(WARNING, 1)`
- Previously both functions returned silently, making the issue invisible

### Node Circle Size Reduction (`graph_renderer_3d.cpp`)

- Node circle radius: `3.0f` → `2.1f` (30% smaller)
- Selection outer ring unchanged (`radius + 2.0f`)

## Modified Files

| File | Changes |
|------|---------|
| `src/render/primitives_3d.h` | `kMaxVertices` 65536 → 524288 |
| `src/render/primitives_3d.cpp` | Added `LOG_FIRST_N(WARNING, 1)` in `AddLine()` and `AddCircle()` overflow paths |
| `src/render/graph_renderer_3d.cpp` | Node circle radius 3.0 → 2.1 |
