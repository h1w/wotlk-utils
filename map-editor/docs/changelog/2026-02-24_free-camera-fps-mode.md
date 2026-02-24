# Free Camera (FPS Mode)

**Date:** 2026-02-24

## Summary

Added a second camera mode to the 3D view — a free/FPS camera with WASD movement, mouse look, and adjustable speed. Toggled with `V` key or via the View menu.

## Problem

The map editor only had an orbital camera for 3D mode, which orbits around a target point. This makes it difficult to navigate through narrow spaces (building interiors, tunnels) or explore the scene from a first-person perspective. There was no way to fly freely through the world.

## Solution

Extended the existing `Camera3D` struct with a `CameraMode` enum (`Orbit` / `Free`) rather than creating a separate class. All renderers continue to receive `const Camera3D&` unchanged because `targetX/Y/Z`, `eyeX/Y/Z`, and `viewProj` are synced in both modes.

### Controls (Free mode)

| Input | Action |
|---|---|
| W / S | Move forward / backward |
| A / D | Strafe left / right |
| Space / Q | Move up |
| Left Ctrl / E | Move down |
| Right-click drag | Mouse look (yaw + pitch) |
| Scroll wheel | Adjust move speed (1.15x per notch) |
| Left Shift (held) | 3x speed boost |
| V | Toggle back to Orbit mode |

### Key design decisions

- **`targetX/Y/Z` sync**: After computing VP in Free mode, `target = eye + lookDir * distance`. All consumers (TileCache, TerrainRenderer, BuildingRenderer, NavmeshRenderer LOD) work unchanged — they load geometry around the look direction.
- **`distance` preserved**: Kept at last orbit-mode value; serves as tile loading radius proxy. Scroll wheel adjusts `moveSpeed` instead.
- **Right-click for both modes**: Consistent interaction — right-click always controls rotation.
- **Pitch range**: Orbit `[-1.5, -0.05]` (always looking down). Free `[-1.5, +1.5]` (full vertical freedom).
- **Smooth transitions**: Free→Orbit computes target ahead of eye and clamps pitch to orbit range; no camera jump.
- **Space key guard**: Space triggers player stop only in Orbit mode (it's vertical movement in Free mode).

### UI additions

- **View menu**: "Orbit Camera" / "Free Camera (FPS)" radio items (shown only in 3D mode).
- **Free Camera panel**: Appears in Free mode with eye coordinates, a logarithmic speed slider (10–2000), and control hints.
- **Status bar**: Shows `"Mode: Free | Eye: (...) | Speed: N"` with tile computed from eye position.

## Files changed

| File | Changes |
|---|---|
| `src/camera/camera3d.h` | Added `CameraMode` enum, `moveSpeed`, free pitch limits, `SetCameraMode()`, private helpers and free-look state |
| `src/camera/camera3d.cpp` | Bifurcated `ComputeMatrices()`, split `ProcessInput()` into `ProcessInputOrbit()`/`ProcessInputFree()`, added `SetCameraMode()`, guarded `UpdateFollow()` |
| `src/app.cpp` | Added `V` hotkey, Space guard, View menu camera items, Free Camera panel with speed slider |
| `src/ui/status_bar.cpp` | Bifurcated `Render3D()` for Free vs Orbit display |

## No changes needed

Renderers (navmesh, terrain, building, ground_plane, primitives), shaders, TileCache, Compass, AppSettings, Canvas.
