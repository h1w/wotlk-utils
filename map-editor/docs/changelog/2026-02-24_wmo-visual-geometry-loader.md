# WMO Visual Geometry Loader — Buildings With Walls!

**Date:** 2026-02-24
**Task:** TASK-004, Phase 1

## Summary

Replaced TrinityCore collision geometry for WMO buildings with **full visual geometry** loaded directly from WoW 3.3.5a MPQ archives. Buildings now render with complete walls, roofs, arches, staircases, towers, and interior rooms. Portal culling provides proper indoor/outdoor visibility — when the camera moves inside a building, interior rooms become visible through portal-connected doorways while hiding distant rooms; from outside, all building geometry is fully visible.

This is the single biggest visual improvement to the map editor since its creation. Stormwind, Ironforge, Undercity, and all other cities now look like actual cities instead of collections of floating roofs.

## Problem

The previous renderer used TrinityCore's pre-extracted **collision geometry** from the `Buildings/` directory. This data is designed for server-side line-of-sight and pathfinding calculations — simplified meshes consisting of large flat rectangles for walls and basic shapes for floors/ceilings. The result: buildings appeared as hollow shells with no walls, no roofs from certain angles, no staircases, no architectural detail. Stormwind looked like a post-apocalyptic ruin.

## Solution

Created `WmoVisualLoader` — a new data loader that reads WMO group files directly from MPQ archives, parses the full MOVT/MOVI/MOPY/MONR geometry chunks, filters collision-only triangles, and feeds the complete visual mesh into the existing `BuildingRenderer` pipeline. The loader integrates as a "visual-first, TC-fallback" strategy: for every WMO spawn, it tries MPQ visual data first; if unavailable, falls back to TC collision geometry. M2 models (props) continue using TC data (deferred to Phase 5).

## New Files

| File | Description |
|------|-------------|
| `src/data/wmo_visual_loader.h` | `WmoGroupVisual` struct (positions, normals, indices, mogpFlags, bbox, Phase 2 stubs), `WmoVisualData` struct, `WmoVisualLoader` class |
| `src/data/wmo_visual_loader.cpp` | Full implementation: MPQ path resolution, root WMO parsing (MOHD nGroups), group file parsing (MOGP/MOPY/MOVI/MOVT/MONR/MOTV/MOBA), MOPY triangle filtering, result caching |

## Modified Files

| File | Change |
|------|--------|
| `src/render/building_renderer.h` | Added `#include "../data/wmo_visual_loader.h"` |
| `src/render/building_renderer.cpp` | Visual-first WMO loading in WorkerLoop; disabled shader wall-culling hack (`heightParams.w = 0`); portal culling now uses group-level BFS visibility |
| `map-editor.vcxproj` | Registered `wmo_visual_loader.h` and `wmo_visual_loader.cpp` |

## Technical Details

### WmoVisualLoader Architecture

```
spawn.modelName (e.g. "Stormwind.wmo")
    │
    ▼
ResolveMpqPath()
    ├── Strategy 1: underscore → backslash (vmapName → MPQ path)
    ├── Strategy 2: case-insensitive basename index from mpq.ListFiles("*.wmo")
    └── Result cached in m_pathCache
    │
    ▼
ParseRootForGroupCount()
    ├── Iterate reversed-tag chunks in root WMO
    ├── Find "DHOM" (MOHD) chunk
    └── Read nGroups at offset +4 within chunk data
    │
    ▼
For each group 0..nGroups-1:
    Build path: {rootStem}_{NNN:03d}.wmo
    │
    ▼
    ParseGroupFile()
        ├── Skip optional MVER chunk
        ├── Read MOGP wrapping chunk ("PGOM")
        │   ├── mogpFlags at offset 0x08
        │   └── bbox at offset 0x0C (6 floats)
        ├── Parse sub-chunks within MOGP body (after 68-byte header):
        │   ├── "YPOM" (MOPY) — 2 bytes/tri: flags + materialId
        │   ├── "IVOM" (MOVI) — uint16 triangle indices
        │   ├── "TVOM" (MOVT) — float3 vertex positions (identity mapping, no coord swap)
        │   ├── "RNOM" (MONR) — float3 normals (stored for Phase 2)
        │   ├── "VTOM" (MOTV) — float2 texcoords (Phase 2 stub)
        │   └── "ABOM" (MOBA) — 24-byte render batches (Phase 2 stub)
        └── MOPY filter: skip triangles with materialId == 0xFF (collision-only)
    │
    ▼
Cache result in m_cache (keyed by vmapModelName)
```

### Coordinate System Discovery

A critical finding during implementation: **TC's vmap4_extractor writes MOVT vertex positions unchanged** — no coordinate transformation is applied to model vertex data. The `fixCoordSystem` function in the TC extractor only applies to ADT spawn placement positions, not to model geometry.

This means WMO group file vertices are in the same coordinate space as TC VMAP model-local coordinates. The existing `TransformVertices()` rotation + VMAP_MID translation math works identically for both visual and collision geometry:

```
Model-local vertex (raw from MOVT, no swap needed)
  → Scale (spawn.scale)
  → Rotate (ZYX Euler matrix from spawn rotation)
  → Translate: wowX = VMAP_MID - (spawn.posX + rx)
               wowY = VMAP_MID - (spawn.posY + ry)
               wowZ = spawn.posZ + rz
```

Three coordinate swap approaches were tested before arriving at the correct identity mapping:
1. `(X, -Z, Y)` — from original plan spec — buildings upside down
2. `(Z, X, Y)` — from TC fixCoordSystem reference — buildings rotated wrong
3. `(X, Y, Z)` — identity, no swap — **correct**, buildings align with navmesh

### MOPY Triangle Filtering

The MOPY chunk contains per-triangle flags (1 byte) and materialId (1 byte). The filter determines which triangles are visible vs collision-only:

- `materialId == 0xFF` → collision-only invisible triangle → **skip**
- All other triangles → **include** (renderable geometry)

An earlier implementation also checked for `flags & 0x20` (F_RENDER flag), but this was too aggressive — many renderable triangles in real WMO files don't have this flag set. Removing this check restored all missing wall geometry.

### Shader Wall-Culling Hack — Disabled

The previous renderer used a shader-based hack for "portal culling from outside": interior group vertices were tagged with `nz = -1.0`, and the pixel shader discarded near-vertical faces (walls) when the camera was outside the WMO. This worked for TC collision data (where "walls" were large flat collision rectangles), but destroyed visual geometry (where walls ARE the real building architecture).

**Before (broken for visual geometry):**
- Interior group vertices tagged `nz = -1.0` in worker thread
- Shader: `if (heightParams.w > 0 && nz == -1 && face is vertical) discard;`
- Result: roofs visible, walls invisible

**After (correct):**
- `heightParams.w = 0.0` always — shader wall hack disabled
- Camera outside: all groups drawn, all faces visible (depth buffer handles occlusion)
- Camera inside: BFS portal culling from camera's group (existing behavior, unchanged)
- Result: full building geometry visible from all angles

A side benefit: when the camera enters a building (moves into a WMO group's bounding box), the BFS portal traversal kicks in, showing only the current room and rooms reachable through visible portal doorways. Interior rooms beyond closed portals remain hidden. Moving the camera inside effectively "opens" the building for inspection.

### BuildingRenderer Integration

The WorkerLoop spawn processing follows a "visual-first, TC-fallback" strategy:

```cpp
for (const auto& spawn : vmapData.spawns) {
    bool isWmo = !(spawn.flags & MOD_M2);
    bool isMpqReady = m_mpq && m_mpq->IsOpen();
    bool gotVisual = false;

    // Try WMO visual geometry from MPQ
    if (isWmo && isMpqReady) {
        const WmoVisualData* visual = wmoVisualLoader.Load(spawn.modelName, *m_mpq);
        if (visual && visual->valid && !visual->groups.empty()) {
            // Transform all group vertices (same math as TC path)
            // Build per-group GroupDrawRange with world-space bbox
            gotVisual = true;
        }
    }

    // Fallback: TC collision geometry
    if (!gotVisual) {
        const BuildingMesh* mesh = buildingLoader.LoadBuilding(spawn.modelName);
        // ... existing TC path ...
    }

    // Portal data loaded for both paths (shared)
}
```

### Geometry Numbers (Stormwind example)

| Metric | TC Collision | MPQ Visual |
|--------|-------------|------------|
| Stormwind.wmo vertices | ~15,000 | ~835,000 |
| Stormwind.wmo triangles | ~12,000 | ~785,000 |
| Visual quality | Flat rectangles, no walls | Complete architecture |

### ResolveMpqPath — Duplicated by Design

The `ResolveMpqPath` function (~80 lines) is duplicated from `WmoPortalLoader`. This is intentional — the two loaders have independent lifecycles (visual loader clears cache on map change, portal loader has its own caching strategy) and are used in different contexts. Merging would create coupling without meaningful benefit.

## Portal Culling Behavior (Updated)

| Camera Position | Rendering Strategy |
|----------------|-------------------|
| Outside all WMOs | All groups drawn; depth buffer handles occlusion; shader wall hack disabled |
| Inside a WMO group | BFS portal traversal from camera's group; connected rooms visible through portals; distant interior rooms hidden |
| Portal Culling OFF | All groups always drawn (toggle in Layer Panel) |

## What This Enables

1. **Stormwind**: Full city with walls, the Mage Tower, Cathedral, Keep, Harbor buildings — all complete
2. **Ironforge**: Complete mountain interior with halls, bridges, lava forge
3. **All WMO buildings**: Inns, barracks, houses, shops — visible from outside AND explorable from inside
4. **Navmesh alignment**: Visual geometry aligns perfectly with navmesh overlay (same coordinate space)
5. **Camera inspection**: Move camera inside any building to see interior rooms, connected via portal culling

## Files Changed Summary

```
NEW:  src/data/wmo_visual_loader.h          — WmoGroupVisual, WmoVisualData, WmoVisualLoader
NEW:  src/data/wmo_visual_loader.cpp        — MPQ path resolution, WMO root/group parsing, MOPY filter
MOD:  src/render/building_renderer.h        — added wmo_visual_loader.h include
MOD:  src/render/building_renderer.cpp      — visual-first loading, shader hack disabled
MOD:  map-editor.vcxproj                    — registered new source files
```

## Deferred to Future Phases

- **Phase 2**: WMO textures & materials (MOTX/MOMT/MOBA per-batch rendering)
- **Smooth normals**: MONR data is loaded and stored but unused; shader uses ddx/ddy flat normals
- **M2 visual geometry**: Props still use TC collision data (Phase 5)
- **ADT-based placements**: Still using TC .vmtile files for spawn positions (Phase 6)
