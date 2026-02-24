# Terrain Smooth toggle

**Date:** 2026-02-24

## Problem

In 3D mode, terrain (from ADT heightmaps) pokes through the navmesh overlay in certain areas — especially on mountain slopes — creating grey "bump" artifacts. The height difference between TC `.map` terrain data and Detour navmesh vertices can be 1-5+ world units on steep slopes, far exceeding what depth bias alone can compensate.

Additionally, terrain always used smooth per-vertex normals (Gouraud shading), making mountains look unnaturally smooth with no way to see the underlying geometry facets.

## Solution

Added a **Smooth** checkbox under Terrain settings in the Layer Panel that controls two rendering behaviors:

### 1. Normal mode (smooth normals vs flat normals)

| Smooth | `heightParams.w` | Normal source | Visual |
|--------|-------------------|---------------|--------|
| OFF    | `-2.0`            | `ddx`/`ddy` derivatives (flat per-triangle) | Faceted terrain, individual triangles visible |
| ON     | `-1.0`            | Per-vertex normals (Gouraud interpolation)   | Smooth terrain surface |

The pixel shader selects normal mode via range check on `heightParams.w`:
- `-1.5 < w < -0.5` → smooth per-vertex normals (terrain smooth ON)
- All other values → flat derivative normals (terrain smooth OFF, buildings)

Building renderer is unaffected (`heightParams.w = 0.0` → always flat normals).

### 2. Slope-dependent Z offset (terrain depression)

When Smooth is ON, the vertex shader pushes terrain geometry downward to prevent it from poking through the navmesh overlay:

```hlsl
float slope = 1.0 - abs(i.norm.z);   // 0=flat, 1=vertical
pos.z += baseColor.a * (1.0 + slope * 5.0);
```

| Surface | `slope` | Offset (`baseColor.a = -1.0`) |
|---------|---------|-------------------------------|
| Flat    | 0.0     | -1.0 unit                     |
| 45 deg  | ~0.3    | -2.5 units                    |
| Steep   | ~0.7    | -4.5 units                    |
| Vertical| 1.0     | -6.0 units                    |

`baseColor.a` is repurposed as the Z offset (unused in PS — alpha is hardcoded to 1.0). Building renderer sets `baseColor.a = 0.0` (no offset).

The offset is applied only to clip-space position; `worldPos` retains the original coordinates for correct lighting calculations.

## Modified Files

| File | Change |
|------|--------|
| `src/render/terrain_pipeline.cpp` | VS: added slope-dependent Z offset via `baseColor.a`. PS: changed normal selection from `heightParams.w < 0` to range check `-1.5 < w < -0.5` for smooth normals |
| `src/render/terrain_renderer.h` | Added `smoothTerrain` public field |
| `src/render/terrain_renderer.cpp` | Set `baseColor.a` (-1.0 or 0.0) and `heightParams.w` (-1.0 or -2.0) based on `smoothTerrain` flag |
| `src/render/building_renderer.cpp` | Changed `baseColor.a` from `1.0` to `0.0` (no Z offset for buildings) |
| `src/render/graph_renderer.h` | Added `terrainSmooth` field to `LayerVisibility` |
| `src/ui/layer_panel.cpp` | Added "Smooth" checkbox under Terrain settings with tooltip |
| `src/data/app_settings.h` | Added `terrainSmooth` field |
| `src/data/app_settings.cpp` | Load/save `terrain_smooth` in JSON |
| `src/app.cpp` | Propagate `terrainSmooth` between settings, layers, and renderer |

## Constant Buffer Layout (TerrainCB, unchanged)

```
float4x4 viewProj     // 64 bytes
float4   lightDir      // xyz=direction, w=ambient
float4   baseColor     // rgb=color, a=Z_OFFSET (was unused alpha)
float4   heightParams  // x=minZ, y=maxZ, z=colorMode, w=NORMAL_MODE
```

`heightParams.w` encoding:
- `-1.0` — terrain, smooth normals (Smooth ON)
- `-2.0` — terrain, flat normals (Smooth OFF)
- `0.0`  — buildings, flat normals (exterior)
- `>0.0` — buildings, flat normals + wall culling (interior)
