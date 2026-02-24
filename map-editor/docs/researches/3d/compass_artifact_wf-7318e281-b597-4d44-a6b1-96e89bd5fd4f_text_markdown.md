# WMO portal culling and indoor visibility for VMAP-based map viewers

**TrinityCore's VMAP4 format preserves `mogpFlags` and `groupWMOID` but discards all portal data (MOPT/MOPR/MOPV), making true portal culling impossible from VMAP files alone.** The best approach without parsing original WMO files from MPQ archives is flag-based filtering: groups with `0x2000` (INTERIOR) set but `0x8` (EXTERIOR) clear are reliably "pure interior" rooms and can be hidden when rendering building exteriors. This heuristic works because the WoW client treats these two flags as orthogonal axes — 0x8 controls the rendering pipeline (world lighting vs. vertex-color lighting) while 0x2000 controls gameplay rules (spell restrictions, ambient sound). Only WebWoWViewerCpp and Wowser have implemented full portal culling among open-source viewers, and both require reading the original WMO data from MPQ, not VMAP extracts.

## How the WoW client's portal culling actually works

The WoW client's WMO visibility system is a **portal-based sector/cell rendering scheme**. Each WMO is divided into groups (sectors), connected by portal polygons placed at doorways, windows, and other openings. The root WMO file stores portal geometry in three chunks: **MOPV** contains portal vertex positions (usually quads), **MOPT** contains 20 bytes per portal with a starting index into MOPV, a vertex count, and a **C4Plane** plane equation (normal + distance), and **MOPR** contains portal references linking each group to its portals. Each MOPR entry stores a `portalIndex` (which portal), `groupIndex` (the group on the other side), and a `side` value (+1 or −1) indicating which side of the portal plane the referencing group occupies.

The algorithm proceeds in three steps. First, the client determines the camera's location by testing it against **BSP trees** (MOBN/MOBR chunks) embedded in each WMO group file — a point-in-BSP test identifies which group, if any, contains the camera. Second, if the camera is outside all groups, the client renders all EXTERIOR-flagged groups and enters any portals visible within the camera frustum. If the camera is inside a specific group, that group renders immediately and the client examines its MOPR portal references. Third, for each visible portal, the client **projects a new frustum** from the camera position through the portal polygon's edges, creating a narrower clipped frustum. The group on the far side of the portal is marked visible, and the process recurses through that group's portals using the clipped frustum. The client enforces **`CWorldView::s_portalMaxDepth = 12`** as the maximum traversal depth.

This frustum-clipping recursion is what makes portal culling so effective — in Ironforge, it can improve framerate from ~4 FPS (all groups rendered) to **55–60 FPS** by eliminating hundreds of invisible interior room groups. The `side` field in MOPR is critical: it tells the traversal which direction to "enter" a portal by comparing `dot(camera_pos, portal.plane.normal) + portal.plane.distance` against the sign of `side`.

## MOGP flags: two orthogonal axes, not a binary switch

The most important finding for practical filtering is that **`0x8` (EXTERIOR) and `0x2000` (INTERIOR) are not mutually exclusive**. They represent different conceptual axes and a single group can have both, neither, or either one set.

**`0x8` (SMOGroup::EXTERIOR)** is a rendering-pipeline flag. Groups with this flag use world directional lighting (sun/moon from the DayNight cycle). The client's `QueryLighting()` function skips MOCV-based vertex color lighting for any group where `flags & (0x8 | 0x40)` is nonzero. This flag broadly correlates with "geometry visible from outside" but is not a geometric containment property — it controls the lighting code path.

**`0x2000` (SMOGroup::INTERIOR)** is a gameplay-and-lighting flag. It tells the renderer to use **only MOCV pre-baked vertex colors** for lighting (no directional sun) and marks the area as "indoors" for spell restrictions (Entangling Roots, mounts). The MOCV alpha channel, after `FixColorVertexAlpha` processing, serves as a blend factor: **alpha = 255** means exterior lighting, **alpha = 0** means interior MOCV lighting, with transition batches retaining intermediate values for smooth doorway blending.

**`0x40` (EXTERIOR_LIT)** acts as an override: if a group has both `0x2000` and `0x40`, exterior lighting is used despite the INTERIOR flag. This creates "indoor-gameplay but outdoor-lit" spaces like covered walkways.

**`0x10000` (ALWAYSDRAW)** has a critical side effect: it **clears `0x8` from both MOGP and MOGI at runtime** during `CMapObjGroup::Create()`. A group stored on disk as `0x10008` becomes `0x10000` at runtime. These are major structural elements (Stormwind's outer walls) that must always render regardless of portal state.

The four practical combinations and their meanings:

- **`0x8` only** (pure exterior): building shell, walls, roofs — visible from outside, world-lit
- **`0x2000` only** (pure interior): enclosed rooms, hallways — MOCV-lit, gameplay-indoor, **geometrically hidden inside the building**
- **`0x2008` (both)**: transitional spaces — porches, archways, covered doorways, open courtyards
- **`0x10000`** (ALWAYSDRAW): major structure always visible, treated as non-exterior at runtime despite file flags

## What TrinityCore's VMAP4 format preserves and discards

The vmap4_extractor reads WMO group files and writes a subset of their data to the VMAP4 binary format. The `GroupModel` class stores exactly these fields per group:

| Field | Preserved | Runtime storage |
|-------|-----------|----------------|
| `mogpFlags` | ✅ Yes | `GroupModel::iMogpFlags` |
| `groupWMOID` | ✅ Yes | `GroupModel::iGroupWMOID` |
| Bounding box | ✅ Yes | `GroupModel::iBound` (AABox) |
| Collision vertices | ✅ Yes (filtered) | `GroupModel::vertices` |
| Collision triangles | ✅ Yes (filtered) | `GroupModel::triangles` |
| Liquid data | ✅ Yes | `GroupModel::iLiquid` (WmoLiquid) |
| RootWMOID | ✅ Yes | `WorldModel::RootWMOID` |
| **Portal data (MOPT/MOPR/MOPV)** | **❌ No** | Discarded entirely |
| **BSP tree (MOBN/MOBR)** | **❌ No** | Replaced by BIH at assembly |
| Group names | ❌ No | Only used for antiportal skip |
| Render materials | ❌ No | Collision-irrelevant |

The extractor's `ConvertToVMAPGroupWmo()` function filters geometry aggressively: only faces where `MOPY[i] & WMO_MATERIAL_COLLISION` is set or where the face is a render face (not detail) are included. Vertices are renumbered via an `IndexRenum` mapping to eliminate unreferenced positions. The binary output per group writes `mogpFlags` and `groupWMOID` as uint32s followed by VERT and TRIM chunks.

The server uses `VMapManager2::getAreaInfo()` to return `mogpFlags` for indoor/outdoor determination at a world position — this is how spell restrictions and mount checks work server-side. The call chain flows through `StaticMapTree → ModelInstance::GetLocationInfo → WorldModel::GetLocationInfo → GroupModel` iteration with BIH spatial queries. **Portal data is never needed server-side** because the server only cares about point-in-group classification, not rendering visibility.

## Reliable flag-based filtering without portal data

Since VMAP files lack portal geometry, the most practical approach uses the preserved `mogpFlags`. The recommended filter for hiding interior-only geometry:

```cpp
bool shouldHideGroup(uint32_t mogpFlags) {
    // Pure interior: INTERIOR set, EXTERIOR clear, not ALWAYSDRAW
    bool pureInterior = (mogpFlags & 0x2000) && !(mogpFlags & 0x8) && !(mogpFlags & 0x10000);
    // Antiportal occlusion geometry
    bool antiportal = (mogpFlags & 0x4000000) != 0;
    // Unreachable decoration
    bool unreachable = (mogpFlags & 0x80) != 0;
    return pureInterior || antiportal || unreachable;
}
```

This catches the vast majority of hidden interior rooms while preserving exterior shells, transitional spaces (0x2008), and ALWAYSDRAW structures. For **more aggressive filtering** that also removes transitional groups like porches and covered walkways, use `(mogpFlags & 0x2000) != 0` — this hides everything the game considers "indoor" regardless of the EXTERIOR flag.

One important caveat: **global WMOs** (instance-map WMOs like Ironforge, Undercity, Deeprun Tram) where the WMO *is* the entire map have nearly all groups flagged INTERIOR. Hiding all interior groups would remove the whole map. Detect these by checking if the WMO's bounding box spans the entire loaded area or if the majority of groups are INTERIOR-flagged, and skip filtering for them.

Supplementary heuristics can refine ambiguous cases. **Bounding box containment analysis** — checking whether a group's AABB is fully contained within the root WMO's AABB with no face touching the root's extremes — can identify deeply nested interior rooms. **Normal direction analysis** — computing the average face normal for a group and checking whether it points outward from the WMO centroid — can distinguish outward-facing shell geometry from inward-facing room walls. These are supplementary to flag filtering, not replacements.

If you have access to **WMOAreaTable.dbc** (extractable from MPQ alongside VMAP data), cross-referencing `RootWMOID` and `groupWMOID` against the DBC's `m_WMOID` and `m_WMOGroupID` columns gives you the game's canonical indoor/outdoor determination via its flags field (bit 0x2 and 0x4 control `CWorldMap::QueryOutdoors`).

## How open-source WoW viewers solve this problem

Only **two** open-source projects have implemented full portal culling, and both read original WMO files from MPQ rather than VMAP extracts:

**WebWoWViewerCpp** (Deamon87) is the most complete implementation. It reads MOPT, MOPR, and MOPV from root WMO files, uses BSP trees from group files for camera location, and performs recursive frustum-clipped portal traversal. The developer documented that portal culling took Ironforge from ~4 FPS to 55–60 FPS. The project also includes a "Disable portal culling" toggle in settings, confirming the feature's maturity.

**Wowser** (wowserhq) implemented portal culling in PR #160, with the most detailed public documentation of the algorithm. The PR describes the full frustum-clipping-through-portals approach, BSP-based camera detection, and vertex color attenuation at portal boundaries.

**Every other viewer** — Noggit, WoW Model Viewer, wow.export, the original WoWMapView, and WoWee — renders all WMO groups without portal culling. The original WoWMapView source explicitly notes: *"Portals could be used for visibility, but I currently have no idea what relations they have to each other or how they work."* Noggit intentionally shows all groups because a map editor needs full visibility. WoWee uses simple frustum + distance culling (160-unit cutoff) without any indoor/outdoor distinction.

## The path forward: parse MPQ or filter by flags

For a VMAP-based map viewer, the practical decision tree is:

**If you only have VMAP data**, flag-based filtering is your best option. The `(mogpFlags & 0x2000) && !(mogpFlags & 0x8) && !(mogpFlags & 0x10000)` test reliably identifies pure interior rooms. Combine this with AABB frustum culling of remaining groups and you'll get a reasonable outdoor-only view of WMO buildings. You will lose transitional geometry (covered porches, doorways) with aggressive filtering, but the exterior shells will be intact.

**If you can parse original WMO files from MPQ**, you gain access to the full portal system. The MOPT, MOPR, and MOPV chunks in the root WMO file, combined with BSP trees in group files, give you everything needed for proper portal culling. This is the approach taken by the only two projects that have solved this problem (WebWoWViewerCpp and Wowser). The WebWoWViewerCpp C++ codebase is the best reference implementation for production use.

**A middle-ground approach** uses VMAP collision meshes for rendering but supplements with portal metadata extracted separately from the original WMO root files. You would need a custom extraction step that reads only the MOPT, MOPR, and MOPV chunks from each WMO root file and writes them as a sidecar file alongside the VMAP data. This avoids full WMO parsing while giving you the connectivity graph needed for portal traversal — though you'd still need BSP data or a point-in-mesh test for camera location detection.

## Conclusion

The WoW WMO portal system is elegantly designed but fundamentally requires data that TrinityCore's VMAP extractor discards. The `mogpFlags` field preserved in VMAP4 files provides the **single most useful signal** for indoor/outdoor classification — specifically, the combination of bits `0x8`, `0x2000`, `0x40`, and `0x10000`. Pure interior groups (`0x2000` set, `0x8` clear) can be hidden with high confidence. But the gap between flag-based heuristics and true portal culling is substantial: in complex WMOs like Ironforge or Stormwind, hundreds of interior groups are invisible from any given camera position, and only recursive frustum-clipped portal traversal can determine which ones to cull. For a serious map viewer, the investment of parsing portal data from the original WMO root files — even as a lightweight supplementary extraction — pays dividends that no amount of flag heuristics can match.