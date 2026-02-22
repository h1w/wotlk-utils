# Compass widget + 3D rendering fixes

**Date:** 2026-02-23

## Summary

Added a compass rose widget to both 2D and 3D viewing modes. Fixed several 3D rendering issues caused by the clip-space X-flip (`XMMatrixScaling(-1,1,1)`) in the VP matrix: pixel shader normals were inverted (causing incorrect lighting), and the compass rotation sign was wrong in 3D mode.

## Compass Widget

### Design

An ImGui overlay rendered via `ImDrawList` (foreground draw list) in the top-right corner of the viewport. Shows cardinal (N/S/E/W) and intercardinal (NE/NW/SE/SW) directions.

- Position: top-right corner, 15px margin, 40px radius
- Semi-transparent dark circle background
- 4 cardinal ticks with labels: **N** (red), S/E/W (white)
- 4 intercardinal ticks (thinner, gray)
- Red triangle pointing toward north
- Small center dot

### Rotation behavior

- **2D mode**: `rotation = 0` — compass is static (north at top, matching north-up canvas)
- **3D mode**: `rotation = -camera.yaw` — compass rotates with camera orbit. Sign is negated to compensate for the VP matrix X-flip.

### API

```cpp
namespace mapedit {
class CompassWidget {
public:
    void Render(float vpX, float vpY, float vpW, float vpH, float rotation);
};
}
```

### Angle convention

Cardinal direction screen angles (measured from screen-up, clockwise positive):
- N = 0 (top)
- E = pi/2 (right)
- S = pi (bottom)
- W = 3pi/2 (left)

Each direction's screen position: `angle = baseAngle + rotation`.

```cpp
dx = sin(angle)    // screen X offset from center
dy = -cos(angle)   // screen Y offset from center
```

## 3D Rendering Fixes

### Pixel shader normal inversion

**Problem:** The VP matrix applies `XMMatrixScaling(-1,1,1)` to flip clip-space X, making east (-Y) appear screen-right. This negates the `ddx()` derivative in the pixel shader, which flips the `cross(ddx, ddy)` normal. Ground-facing triangles got normals pointing downward instead of upward, causing incorrect directional lighting.

**Fix:** Negate the computed normal in the pixel shader:

```hlsl
float3 dpdx = ddx(i.worldPos);
float3 dpdy = ddy(i.worldPos);
float3 N = -normalize(cross(dpdx, dpdy));  // negate for X-flip compensation
```

### Camera orbit/pan directions

The VP matrix X-flip already reverses the visual output on screen. The orbit and pan input signs do NOT need compensation — the original signs produce correct grab-and-drag behavior because the X-flip provides the visual reversal automatically.

## Files changed

| File | Change |
|------|--------|
| `src/ui/compass.h` | **New.** CompassWidget class declaration |
| `src/ui/compass.cpp` | **New.** Compass rose rendering via ImDrawList |
| `src/app.h` | Add `#include "ui/compass.h"` + `CompassWidget m_compass` member |
| `src/app.cpp` | Call `m_compass.Render()` in RenderFrame2D (rotation=0) and RenderFrame3D (rotation=-yaw) |
| `src/render/navmesh_pipeline_3d.cpp` | Negate pixel shader flat normal to compensate for X-flip |
| `map-editor.vcxproj` | Add compass.h and compass.cpp to build |
