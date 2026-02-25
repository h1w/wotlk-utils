# Camera Position Sync Between 2D/3D + Terrain Height in Status Bar

**Date:** 2026-02-25

## Summary

Camera position (XY) now persists when switching between 2D and 3D orbit mode. The status bar at the bottom of the window now displays real terrain height under the cursor in both 2D and 3D modes.

## Changes

### New: View mode camera sync (`src/app.h`, `src/app.cpp`)

- **`App::SetViewMode(ViewMode)`** — centralized method for all 2D/3D mode switches
- **2D → 3D (orbit)**: copies `canvas.centerX/Y` → `camera3d.targetX/Y`
- **3D (orbit) → 2D**: copies `camera3d.targetX/Y` → `canvas.centerX/Y`
- **Free camera**: no position sync in either direction — free camera is fully independent
- All 4 switch points now use `SetViewMode()`:
  - View menu "Mode: 2D" / "Mode: 3D"
  - Keyboard shortcuts Ctrl+1 / Ctrl+2

### Edit: Status bar terrain height (`src/ui/status_bar.h`, `src/ui/status_bar.cpp`)

- **`Render()`** and **`Render3D()`** now accept optional `TerrainHeightSampler*` parameter
- **2D mode**: samples height at cursor world position from `ScreenToWorld`
- **3D mode**: ray-casts cursor through viewport via `ScreenToRay`, intersects with horizontal plane at `targetZ` (orbit) or `eyeZ - 50` (free), samples height at intersection point
- Displays `H: 123.4` or `H: ---` (if tile not loaded) at the end of the status bar text
- Helper `FormatHeight()` encapsulates the sampling + formatting logic

### Edit: App render calls (`src/app.cpp`)

- `m_statusBar.Render()` and `m_statusBar.Render3D()` now pass `&m_heightSampler`

## Modified Files

| File | Action | Changes |
|------|--------|---------|
| `src/app.h` | EDIT | Added `SetViewMode()` declaration |
| `src/app.cpp` | EDIT | `SetViewMode()` implementation, replaced 4 direct `m_viewMode` assignments, pass `&m_heightSampler` to status bar |
| `src/ui/status_bar.h` | EDIT | Added `TerrainHeightSampler*` parameter to both `Render` methods |
| `src/ui/status_bar.cpp` | EDIT | `FormatHeight()` helper, height display in all 3 status bar modes, ray-cast for 3D cursor position |
