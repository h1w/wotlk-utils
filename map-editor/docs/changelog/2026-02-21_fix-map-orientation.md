# Fix map orientation and minimap tile alignment

**Date:** 2026-02-21

## Summary

Fixed two rendering bugs that caused the map editor to display the world map incorrectly:
1. Minimap BLP tiles were offset by 1 tile (~533 yards) in both axes from their correct world positions
2. The entire map was vertically mirrored (north at bottom, south at top)

## Bug 1: Minimap tile offset (1 tile in both axes)

**Symptom:** Graph nodes (flight masters, portals, boat docks) appeared shifted ~533 yards from their expected positions on the minimap imagery. For example, the Stormwind fountain showed coordinates (-8286, 1167) when hovering over the minimap image, but the real coordinates are (-8830, 640).

**Root cause:** WoW minimap BLP tiles use a different grid origin than ADT/navigation tiles. The BLP image for tile index (tx, ty) covers a world area that is 1 tile north and 1 tile east of what the formula `(32 - tx) * tileSize` computes.

**Fix:** Changed tile bounds in `minimap_cache.cpp` Render():
```cpp
// Before (incorrect):
float worldMinX = (32 - tx) * kTileSize;
float worldMaxX = (33 - tx) * kTileSize;
float worldMinY = (32 - ty) * kTileSize;
float worldMaxY = (33 - ty) * kTileSize;

// After (correct):
float worldMinX = (31 - tx) * kTileSize;
float worldMaxX = (32 - tx) * kTileSize;
float worldMinY = (31 - ty) * kTileSize;
float worldMaxY = (32 - ty) * kTileSize;
```

**Verification:** Stormwind fountain now reads (-8820, 634), within ~10 yards of the known position (-8830, 640).

## Bug 2: Vertical map mirror (north/south flipped)

**Symptom:** The map was rendered upside-down compared to the in-game WoW map. Booty Bay (south) appeared at the top of the screen, Plaguelands (north) at the bottom.

**Root cause:** The `WorldToScreen` formula assumed WoW X+ = south, but actual coordinate data shows X+ = north (Plaguelands X ~ +1600, Stormwind X ~ -8830, Booty Bay X ~ -14400). The formula `sy = vpCenterY + (wowX - centerX) * zoom` placed larger X (north) at the bottom of the screen.

**Fix:** Negated the X component in the coordinate transform pipeline:

`canvas.cpp` — WorldToScreen:
```cpp
// Before: sy = vpCenterY + (wowX - centerX) * zoom
// After:  sy = vpCenterY + (centerX - wowX) * zoom
```

`canvas.cpp` — ScreenToWorld (inverse):
```cpp
// Before: wowX = centerX + (sy - cy) / zoom
// After:  wowX = centerX - (sy - cy) / zoom
```

Pan and zoom input handling updated to match the new sign convention.

All rectangle-based renderers updated to swap min/max X corners for correct screen ordering:
- `minimap_cache.cpp` Render()
- `grid_renderer.cpp` RenderTileGrid()
- `map_background.cpp` Render()

Minimap tile UV changed from V-flip `(0,1)->(1,0)` to direct `(0,0)->(1,1)` since BLP row 0 = north now matches screen top = north.

**Verification:** Eastern Kingdoms map now matches in-game orientation — Plaguelands at top (north), Booty Bay at bottom (south). All node positions correctly overlay the minimap imagery.

## Files changed

| File | Change |
|------|--------|
| `src/canvas/canvas.cpp` | Negate X in WorldToScreen/ScreenToWorld, fix pan/zoom signs |
| `src/canvas/canvas.h` | Fix comment: `WoW X (south+)` -> `WoW X (north+)` |
| `src/render/minimap_cache.cpp` | 1-tile offset fix + corner swap + remove V-flip UV |
| `src/render/grid_renderer.cpp` | Corner swap for tile rectangles |
| `src/render/map_background.cpp` | Corner swap for background image |
| `src/ui/layer_panel.cpp` | No functional change (diagnostic code added then removed) |
| `src/ui/layer_panel.h` | No functional change (diagnostic code added then removed) |

## Notes

- **Update (2026-02-22):** The 1-tile offset is NOT specific to minimap BLP tiles — it applies to ALL tile data (navmesh `.mmtile` files too). The correct tile-to-world formula is `wowX_min = (31 - tileX) * 533.33`, not `(32 - tileX)`. The navmesh tile grid, UI tile coordinates, and background bounds were fixed in a follow-up patch. See [2026-02-22_fix-navmesh-tile-offset.md](2026-02-22_fix-navmesh-tile-offset.md).
- The coordinate grid and ScreenToWorld (status bar coordinates) remain correct and consistent with the new orientation.
