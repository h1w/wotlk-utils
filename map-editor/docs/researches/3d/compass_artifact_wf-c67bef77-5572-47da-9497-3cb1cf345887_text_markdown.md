# WoW WMO portal culling and group visibility explained

**WoW's WMO rendering uses a BSP-tree-based camera containment test combined with portal-graph traversal to determine which groups to draw.** When the camera is outside all groups, every EXTERIOR-flagged group renders immediately, plus the client traverses portals inward through doorways to reveal interior groups visible in the frustum. The `ALWAYSDRAW` flag (0x10000) exists specifically for groups like Stormwind building shells that must always render regardless of portal state. Below is a complete technical breakdown covering the 3.3.5a/WotLK-era format and rendering behavior, drawn from wowdev.wiki documentation, the Wowser portal culling design (fallenoak/Deamon87), TrinityCore vmap code, and community reverse engineering.

---

## How the client determines camera inside vs. outside a WMO

The WoW client uses a **two-stage spatial query** to determine whether the camera falls inside a specific WMO group. The first stage is a simple **AABB bounding box test** against each group's bounding box (stored in both MOGI in the root file and the MOGP header of each group file). Bounding boxes of WMO groups frequently overlap — especially in complex structures like Ironforge — so the AABB test alone is insufficient.

The second, definitive stage uses the **BSP tree** baked into each WMO group (MOBN/MOBR chunks, present when MOGP flag **0x1** is set). This BSP tree performs a point-in-polygon-soup test to confirm whether the camera position actually lies within the group's enclosed geometry. Deamon87 originated this approach for his WebWoWViewer, and Skarn confirmed it empirically on wowdev.wiki (April 2022): *"There was a common misconception that this BSP is only used for collision purposes. In fact, it is also used by the client to determine if you are currently inside the WMO group. If any faces are missing from the BSP, indoor groups will be culled on approaching them."*

The BSP tree thus serves a **dual purpose**: collision detection and camera containment. Collision-only faces (material ID `0xFF` in MOPY) must also be present in the BSP for correct culling. The BSP node structure uses axis-aligned split planes (`planeType` 0=YZ, 1=XZ, 2=XY) with `planeType=4` marking leaf nodes. After the BSP test, the camera is classified as either **outside all WMO groups** (overworld) or **inside one specific group**. The MCVP chunk (convex volume planes) provides an additional containment test primarily used for transport WMOs.

The camera location then drives a critical branching rule encoded in the EXTERIOR flag documentation: *"If camera is AABB present in a group with [EXTERIOR] flag, and not present in any group with SMOGroup::INTERIOR, render all exteriors."*

---

## The exterior case: what renders when the camera is outside

When the camera is **outside all WMO groups**, the client executes a three-part rendering strategy. First, **all groups with the EXTERIOR flag (0x8) are unconditionally rendered** — these form the outer shell and terrain of the WMO. Second, **all ALWAYSDRAW groups (0x10000) are unconditionally rendered** — these are interior-lit groups that must always be visible (more on this below). Third — and this is critical — **the client does perform portal traversal from outside**.

Portal traversal from the exterior works as follows. The client identifies all portal polygons that **face the exterior world** (portals connecting exterior groups to interior groups, such as doorways and archways). Each of these portals is tested against the **current camera frustum**. For portals that pass the frustum test, the client creates a **new frustum clipped to the portal polygon's edges** — projecting the portal's screen-space silhouette from the camera position into a narrower viewing cone. The interior group on the other side of the portal is marked visible if its bounding box intersects this clipped frustum. The traversal then **recurses** through that interior group's own portals, further clipping the frustum at each step. This progressive narrowing dramatically reduces overdraw.

The recursion depth is bounded by **`CWorldView::s_portalMaxDepth = 12`**, preventing runaway traversal in complex WMOs. This means interior rooms visible through a doorway chain up to 12 portals deep can be discovered and rendered from outside.

So the answer is definitively **not** "render exterior groups only." From outside, the client renders: all EXTERIOR groups + all ALWAYSDRAW groups + any interior groups reachable through portal traversal within the camera frustum.

---

## How Stormwind buildings stay visible: the ALWAYSDRAW mechanism

The question of how city WMOs like Stormwind handle buildings that are interior spaces but must be visible from the street is answered by **`SMOGroup::ALWAYSDRAW` (flag 0x10000)**. This flag's behavior, documented on wowdev.wiki and corroborated by the OwnedCore Model Editing Compendium (which specifically notes *"0x10000 — Used in Stormwind?"*), works through a clever two-step mechanism.

In the **raw WMO file data**, groups intended to always be visible have both `0x8` (EXTERIOR) and `0x10000` (ALWAYSDRAW) set. During **`CMapObjGroup::Create()` at runtime**, the client **clears the EXTERIOR flag (0x8)** from groups that have ALWAYSDRAW set. The result: the group loses its EXTERIOR flag (so the lighting system treats it as interior, using MOCV vertex colors and ambient color from the WMO header), but the ALWAYSDRAW flag ensures it **bypasses normal portal culling entirely** and is always included in the render set.

This is distinct from portal traversal. Relying on portal traversal alone to show building exteriors would cause visual popping as doorway portals enter and exit the camera frustum. ALWAYSDRAW guarantees the building shell geometry is always drawn. The interior rooms deeper inside — the back rooms of shops, upstairs areas — are then discovered through portal traversal when the camera can see through the door.

Buildings are **not** simply flagged as EXTERIOR despite being indoor spaces. They are genuinely interior-lit groups (using prebaked vertex colors) that are forced to always render via ALWAYSDRAW. This is why building facades in Stormwind have that characteristic WMO interior look with baked lighting even when viewed from the sunlit street.

---

## Complete MOGP flag reference for visibility decisions

The following flags directly control or influence group visibility and rendering:

| Flag | Name | Visibility role |
|------|------|----------------|
| **0x1** | HAS_BSP | Enables point-in-group containment test via BSP tree — required for indoor groups |
| **0x8** | EXTERIOR | Group renders when camera is outside; marks group as outdoor for lighting |
| **0x40** | EXTERIOR_LIT | Uses exterior directional lighting even on INTERIOR groups; skips MOCV queries |
| **0x80** | UNREACHABLE | Group is never rendered or collided; skipped by TrinityCore vmap extraction |
| **0x100** | (unnamed) | Shows exterior sky inside an interior group (used in stratholme_past.wmo city interiors) |
| **0x2000** | INTERIOR | Indoor group; triggers portal-based culling when camera is inside |
| **0x10000** | ALWAYSDRAW | Always rendered; clears EXTERIOR (0x8) at runtime for interior lighting |
| **0x40000** | SHOW_SKYBOX | Renders the MOSB skybox for this group; auto-cleared if no MOSB chunk |
| **0x4000000** | ANTIPORTAL | Creates occluder geometry from group triangles; requires UNREACHABLE flag |

Groups can combine INTERIOR (0x2000) with EXTERIOR_LIT (0x40) to create spaces that are topologically interior (reached through portals, use portal culling) but lit by the sun/directional light rather than baked vertex colors. Flag 0x100 is a separate visual override that displays the exterior sky dome inside an interior group without changing the culling topology.

There is **no separate "DO_NOT_USE_LOCAL_DIFFUSE_LIGHTING" flag** beyond EXTERIOR_LIT (0x40) — that description *is* the EXTERIOR_LIT flag. The `CMapObj::QueryLighting()` function confirms this: it returns 0 (skip interior lighting query) for any group with either EXTERIOR or EXTERIOR_LIT set.

---

## Portal graph structure: MOPT, MOPR, MOPV, and the traversal topology

The portal system is defined across three root-file chunks that together form a **bidirectional graph** connecting WMO groups through portal polygon doorways.

**MOPV** (Portal Vertices) stores `C3Vector` positions — the actual 3D coordinates of portal polygon corners in WMO model space. **MOPT** (Portal Definitions) defines each portal as an `SMOPortal` structure: a `startVertex` index into MOPV, a vertex `count` (usually 4 for quads, but up to 10 for complex shapes like the Ironforge archway), and a `C4Plane` plane equation (normal + distance). There is a **hardcoded maximum of 128 portals** per WMO. **MOPR** (Portal References) connects portals to groups via `SMOPortalRef` structures containing a `portalIndex`, the `groupIndex` of the group on the other side, and a `side` field (positive or negative) indicating which side of the portal plane the referencing group occupies.

Each MOGP header stores `mopr_index` and `mopr_count` — the range of entries in the MOPR chunk that represent portals **leading out of** that group. The total MOPR entry count equals the sum of all groups' `mopr_count` values, and is roughly **twice the number of portals** because each portal is typically referenced from both sides. This bidirectional structure allows the traversal algorithm to enter a portal from either group.

The traversal algorithm uses the portal plane's `side` field combined with a dot product of the camera-to-portal vector against the portal normal to determine **which direction** the camera is looking through the portal. Only portals whose geometry is facing the camera and intersecting the current (possibly clipped) frustum are entered. The frustum clipping at each portal step works by projecting the portal polygon vertices to screen space, computing a bounding region, and constructing new frustum planes from the camera position through the portal's projected edges.

---

## Pseudocode for the complete portal culling algorithm

The reconstructed algorithm, based on fallenoak's Wowser design (PR #160, Issue #150) with Deamon87's guidance:

```
function renderWMO(camera, wmo):
    cameraGroup = findCameraGroup(camera, wmo)  // BSP test per group
    visibleGroups = {}
    
    if cameraGroup is NONE (exterior):
        // Phase 1: Render all exterior + always-draw groups
        for group in wmo.groups:
            if group.flags & EXTERIOR or group.flags & ALWAYSDRAW:
                visibleGroups.add(group)
        
        // Phase 2: Enter portals facing exterior within camera frustum
        for group in visibleGroups where EXTERIOR:
            for portalRef in group.portalRefs:
                portal = wmo.portals[portalRef.portalIndex]
                if portal is facing outward AND intersects(camera.frustum, portal.polygon):
                    clippedFrustum = clipFrustumToPortal(camera.pos, portal.polygon)
                    traversePortals(portalRef.targetGroup, clippedFrustum, depth=0)
    
    else:  // Camera inside a specific group
        visibleGroups.add(cameraGroup)
        for portalRef in cameraGroup.portalRefs:
            portal = wmo.portals[portalRef.portalIndex]
            if intersects(camera.frustum, portal.polygon):
                clippedFrustum = clipFrustumToPortal(camera.pos, portal.polygon)
                traversePortals(portalRef.targetGroup, clippedFrustum, depth=0)

function traversePortals(group, frustum, depth):
    if depth >= 12: return                    // s_portalMaxDepth = 12
    if not intersects(frustum, group.boundingBox): return
    visibleGroups.add(group)
    for portalRef in group.portalRefs:
        portal = wmo.portals[portalRef.portalIndex]
        if intersects(frustum, portal.polygon):
            narrowerFrustum = clipFrustumToPortal(camera.pos, portal.polygon)
            traversePortals(portalRef.targetGroup, narrowerFrustum, depth + 1)
```

Doodad visibility follows the same traversal: each doodad is only rendered if its bounding box intersects the clipped frustum at the group level where it resides.

---

## Practical implications for a collision geometry editor

For a 3D map editor rendering WMO collision geometry from TrinityCore vmap data, **you do not need to implement the full portal culling algorithm** unless you want accurate visibility matching the game client. A simpler approach for an editor:

- **Render all groups by default** — this gives full visibility of the WMO, which is what an editor typically wants.
- **Optional portal culling**: If you want to replicate client behavior, check MOGP flags for each group. Groups with EXTERIOR (0x8) or ALWAYSDRAW (0x10000) render unconditionally. For interior groups, implement a simplified portal test: check if the camera can see through any portal polygon connecting to the group.
- **TrinityCore vmap data includes `mogpFlags`** in each `GroupModel` (stored alongside the BSP tree and collision triangles). The server uses flag 0x8 for indoor/outdoor determination (`vmap.enableIndoorCheck`) but does **not** implement portal culling — it only uses ray-triangle intersection for line-of-sight checks.
- **BSP trees from MOBN/MOBR** can be used for your own point-in-group tests if you need camera containment detection.
- **Antiportal groups** (flag 0x4000000 or named "antiportal") and **UNREACHABLE groups** (flag 0x80) should be skipped entirely — TrinityCore's `WMOGroup::ShouldSkip()` confirms this.

## Conclusion

WoW's WMO visibility system is a textbook portal-culling implementation with one key WoW-specific addition: the **ALWAYSDRAW flag** that solves the problem of interior-lit building shells in open-world city WMOs. The BSP tree is the linchpin — it answers "is the camera inside this group?" which drives the entire rendering decision tree. From outside, the client renders all exterior groups immediately, always renders ALWAYSDRAW groups, and performs recursive portal traversal through doorways with progressive frustum clipping up to 12 levels deep. The portal graph in MOPT/MOPR/MOPV is bidirectional, with each portal referenced from both adjacent groups, and the side field encoding directionality. For a map editor, the most pragmatic approach is rendering all groups unconditionally and optionally layering portal culling on top for testing or visualization purposes.