# Navmesh pathfinding with hostile NPC avoidance in Detour

**Area cost marking via `setPolyArea` + `setAreaCost` is the optimal approach** when `DT_VIRTUAL_QUERYFILTER` is unavailable. It avoids the fatal `findNearestPoly` failure that flag exclusion causes, works with pre-generated TrinityCore mmaps without tile rebuilding, and degrades gracefully when no bypass exists. The key architectural insight is using **two separate `dtQueryFilter` instances**: a permissive filter for polygon lookup and a danger-aware filter for pathfinding. Combined with `closestPointOnPoly` for precise circular zone marking and an RAII restoration pattern, this approach handles all edge cases while staying well under performance budgets — the entire mark-path-restore cycle costs under **500μs** on a 3×3 tile grid with 30 NPCs.

## Why area cost marking wins over every alternative

Seven approaches were evaluated against the critical constraint (non-virtual `passFilter`/`getCost`). Here is why each alternative falls short and area cost marking stands alone:

**Flag exclusion (`setPolyFlags` + `setExcludeFlags`)** is the most tempting alternative because excluded polygons are never expanded by A*, making the search faster. But it has a **fatal flaw**: `findNearestPoly` applies the filter, so if the player stands inside a danger zone, `findNearestPoly` returns `ref=0` and all subsequent pathfinding fails. Detour's `findPath` also checks `passFilter()` on `startRef` before beginning the search — with excluded flags, it returns `DT_FAILURE | DT_INVALID_PARAM`. There is no workaround within the non-virtual filter constraint that doesn't involve using two different filter configurations, and even then flag exclusion offers only binary pass/block behavior with no graceful degradation.

**dtTileCache with temporary obstacles** is architecturally incompatible. TrinityCore mmaps are stored as final `dtNavMeshData` blobs (output of `dtCreateNavMeshData`). dtTileCache requires compressed intermediate layer data — a completely different format containing compact heightfield layers. Retrofitting would require re-generating every tile from source VMAP geometry, effectively replacing the entire mmap system. Not viable for an injected DLL.

**Post-processing (build normal path, shift waypoints)** cannot reroute around danger zones. It nudges waypoints perpendicular to the path direction, but shifted waypoints may land off the navmesh, on the wrong side of walls, or still inside overlapping danger zones. It also cannot discover a topologically different route that the normal A* didn't consider.

**dtCrowd / dtObstacleAvoidance** operates at the velocity/steering layer, not the path-planning layer. It is designed for moving agents avoiding each other in real time (RVO). For stationary hostile NPCs that need *route-level* avoidance, it's the wrong abstraction. GitHub issue #482 confirms dtCrowd's topology optimization can override high-cost areas, causing agents to cut through danger zones.

**Forking Detour** to add a custom `findPath` is technically viable but creates a maintenance burden. Every vcpkg update requires merging changes, and the actual benefit over area cost marking is minimal — you'd essentially reimplement what area costs already provide.

**The two-pass approach** (try flag exclusion first, fall back to area costs) doubles the work in the common failure case and adds complexity without meaningful benefit over pure area cost marking.

## The recommended algorithm in detail

The architecture uses area ID **63** (TrinityCore uses only areas 8–11, leaving 12–63 completely free) as the danger designation, two filter instances, and an RAII guard for navmesh restoration.

```cpp
// Constants
constexpr unsigned char AREA_DANGER = 63;
constexpr float DANGER_COST = 50.0f;  // See cost tuning section below
constexpr float AGGRO_MARGIN = 1.05f; // 5% safety margin on aggro radius

// Two filters: one permissive, one danger-aware
dtQueryFilter findPolyFilter;   // For findNearestPoly — always succeeds
findPolyFilter.setIncludeFlags(0xFFFF);
findPolyFilter.setExcludeFlags(0);
// All areaCosts remain 1.0 (default)

dtQueryFilter pathFilter;       // For findPath — penalizes danger zones
pathFilter.setIncludeFlags(0xFFFF);
pathFilter.setExcludeFlags(0);
pathFilter.setAreaCost(AREA_DANGER, DANGER_COST);
// All other areaCosts remain 1.0 (default)
```

The **dual-filter pattern** is the core innovation. `findNearestPoly` uses the permissive filter so the player's polygon is always found, even inside a danger zone. `findPath` uses the danger-aware filter to route around high-cost areas. This works because `findPath` calls `passFilter()` on `startRef`, and since area cost marking doesn't change polygon flags — only the area type — the polygon always passes the include/exclude check. The cost penalty takes effect during A* edge expansion, not during the initial validation.

### RAII polygon restoration

```cpp
struct PolyBackup { dtPolyRef ref; unsigned char originalArea; };

class PolyAreaGuard {
    dtNavMesh* m_nav;
    std::vector<PolyBackup> m_backups;
public:
    PolyAreaGuard(dtNavMesh* nav) : m_nav(nav) {}
    
    void markDanger(dtPolyRef ref) {
        unsigned char origArea;
        if (dtStatusSucceed(m_nav->getPolyArea(ref, &origArea))) {
            if (origArea != AREA_DANGER) {  // Avoid duplicate saves
                m_backups.push_back({ref, origArea});
                m_nav->setPolyArea(ref, AREA_DANGER);
            }
        }
    }
    
    ~PolyAreaGuard() {
        for (auto& b : m_backups)
            m_nav->setPolyArea(b.ref, b.originalArea);
    }
};
```

### Complete pathfinding function

```cpp
PathResult findPathAvoidingNPCs(
    dtNavMeshQuery* query, dtNavMesh* navMesh,
    const float* start, const float* end,
    const std::vector<AggroZone>& zones,  // {center[3], radius}
    dtPolyRef* pathPolys, int maxPath,
    float* straightPath, int maxStraight)
{
    PolyAreaGuard guard(navMesh);
    
    // ── Step 1: Mark danger polygons ──
    const float polySearchExtents[3] = {0.5f, 2.0f, 0.5f};  // For closestPointOnPoly
    
    for (const auto& zone : zones) {
        float halfExtents[3] = {
            zone.radius * AGGRO_MARGIN,
            10.0f,  // Generous Y to catch multi-level areas
            zone.radius * AGGRO_MARGIN
        };
        
        dtPolyRef polys[256];
        int polyCount = 0;
        query->queryPolygons(zone.center, halfExtents,
                             &findPolyFilter, polys, &polyCount, 256);
        
        float rSq = (zone.radius * AGGRO_MARGIN) * (zone.radius * AGGRO_MARGIN);
        
        for (int i = 0; i < polyCount; ++i) {
            // Precise circle test via closestPointOnPoly
            float closestPt[3];
            bool posOverPoly;
            query->closestPointOnPoly(polys[i], zone.center,
                                      closestPt, &posOverPoly);
            
            float dx = closestPt[0] - zone.center[0];
            float dz = closestPt[2] - zone.center[2];
            if (dx*dx + dz*dz <= rSq) {
                guard.markDanger(polys[i]);
            }
        }
    }
    
    // ── Step 2: Find start/end polygons (permissive filter) ──
    float nearestPt[3];
    const float lookupExtents[3] = {3.0f, 5.0f, 3.0f};
    
    dtPolyRef startRef = 0, endRef = 0;
    query->findNearestPoly(start, lookupExtents,
                           &findPolyFilter, &startRef, nearestPt);
    query->findNearestPoly(end, lookupExtents,
                           &findPolyFilter, &endRef, nearestPt);
    
    if (!startRef || !endRef)
        return PathResult::NO_POLY;
    
    // ── Step 3: A* pathfinding (danger-aware filter) ──
    int pathCount = 0;
    dtStatus status = query->findPath(startRef, endRef, start, end,
                                       &pathFilter, pathPolys,
                                       &pathCount, maxPath);
    
    if (dtStatusFailed(status))
        return PathResult::FAILED;
    
    bool isPartial = (status & DT_PARTIAL_RESULT) != 0;
    bool throughDanger = false;
    
    // ── Step 4: Convert to straight path ──
    int straightCount = 0;
    unsigned char straightFlags[64];
    dtPolyRef straightPolys[64];
    query->findStraightPath(start, end, pathPolys, pathCount,
                            straightPath, straightFlags, straightPolys,
                            &straightCount, maxStraight);
    
    // ── Step 5: Check if path traverses danger ──
    for (int i = 0; i < pathCount; ++i) {
        unsigned char area;
        navMesh->getPolyArea(pathPolys[i], &area);
        if (area == AREA_DANGER) {
            throughDanger = true;
            break;
        }
    }
    
    // Guard destructor restores all polygon areas here
    
    if (isPartial)
        return PathResult::PARTIAL;
    if (throughDanger)
        return PathResult::THROUGH_DANGER;  // Caller decides: engage or wait
    return PathResult::SAFE;
}
```

## How Detour A* actually behaves with high costs

Understanding Detour's `findPath` internals is essential for choosing the right cost value and handling edge cases correctly. The implementation in `DetourNavMeshQuery.cpp` uses a standard A* over the polygon adjacency graph with f = g + h, where **g accumulates `dtVdist(pa, pb) × m_areaCost[area]`** along the path, and **h = `dtVdist(nodePos, endPos) × 0.999f`** (the `H_SCALE` constant ensures admissibility).

There is **no cost cutoff** in the code. Detour never checks for maximum accumulated cost, never calls `isinf()` or `isnan()`, and never saturates values. The implications for different cost values are significant:

**Cost = 50.0** (recommended): A 10-unit edge through danger costs 500. A typical path accumulating 1000 units of danger traversal reaches g ≈ 50,000. Float precision is excellent at this magnitude (~7 significant digits), and A* directional guidance from the heuristic remains effective because h and g are in similar ranges. The pathfinder will take detours up to roughly **50× the direct distance** through a danger zone before choosing to go through it.

**Cost = 100,000**: A 10-unit danger edge costs 1,000,000. After traversing several such edges, accumulated g reaches ~10⁹. At this scale, float comparison loses ~5 digits of precision — two paths differing by less than 100 in total cost become indistinguishable. The heuristic contribution becomes noise relative to g, causing the search to degenerate toward Dijkstra-like behavior (expanding in all directions). This dramatically **increases node consumption**, making `DT_OUT_OF_NODES` much more likely.

**Cost = FLT_MAX (~3.4e38)**: Any non-zero edge produces `inf`. Since `inf + anything = inf`, all paths through danger become cost-equivalent in the priority queue. This wastes node pool capacity as A* explores danger-zone nodes in arbitrary order without being able to distinguish between them. Effectively blocks traversal but less cleanly than flag exclusion and with worse node pool utilization.

The **`maxNodes` parameter** passed to `dtNavMeshQuery::init()` caps the node pool. The node index type `dtNodeIndex` is `unsigned short`, limiting the theoretical maximum to **65,535**. When the pool is exhausted, `getNode()` returns NULL and that neighbor is silently skipped. If the search stalls, it returns `DT_PARTIAL_RESULT | DT_OUT_OF_NODES`. With high area costs, A* explores many cheap-area nodes before considering expensive ones, so node exhaustion is a real risk if `maxNodes` is too low. For a 3×3 tile grid with ~500 polygons, **maxNodes = 2048** is sufficient; for larger search spaces, use **8192**.

### Partial result behavior

When `findPath` cannot reach the goal (open list or node pool exhausted), it traces the path backward from `lastBestNode` — the node with the **lowest f-score** ever seen during the search. With high-cost danger areas, this node is typically the last cheap-area node before the danger zone boundary, because nodes beyond the danger zone have enormously inflated f-scores. This means **partial results tend to end just before the danger zone**, which is exactly what you want — the bot walks to the edge of safe territory and stops, rather than ending at an arbitrary point.

However, this is not guaranteed. If the goal is behind a large danger zone and there are cheap nodes in a different direction, `lastBestNode` might point away from the goal entirely. Always validate partial paths against the intended direction of travel.

## Precise circular zone marking on an AABB query

`queryPolygons` returns all polygons whose bounding boxes intersect the query AABB. For circular aggro zones, this produces a superset that includes corner polygons outside the circle. Three filtering approaches were evaluated:

**Polygon center distance** (cheapest, least accurate): Average all vertices to find the polygon centroid, then check 2D distance to zone center. Misses large polygons whose centroid is outside the radius but whose area overlaps the circle. On coarse navmeshes (typical of TrinityCore open-world terrain), this can miss critical boundary polygons.

**Vertex-only check** (fast, still imprecise): Check if any polygon vertex falls within the circle. Misses the case where an edge passes through the circle but both endpoints are outside it — geometrically possible for large polygons straddling the zone boundary.

**`closestPointOnPoly` test** (recommended): Call `dtNavMeshQuery::closestPointOnPoly(polyRef, zoneCenter, closestPt, &posOverPoly)` for each candidate polygon. This returns the nearest point on the polygon's surface to the query point, using Detour's own internal geometry. If `dist2D(closestPt, zoneCenter) <= aggroRadius`, the polygon overlaps the circle. This handles all edge cases — large polygons, edge intersections, point containment — with a single Detour API call. The overhead is minimal: `closestPointOnPoly` is O(V) where V is the vertex count per polygon (max 6 for Detour), and you're calling it only on the AABB-filtered subset.

```cpp
// Precise polygon-circle intersection
float closestPt[3];
bool posOverPoly;
query->closestPointOnPoly(polyRef, zoneCenter, closestPt, &posOverPoly);

float dx = closestPt[0] - zoneCenter[0];
float dz = closestPt[2] - zoneCenter[2];  // Detour uses Y-up; XZ is the ground plane
bool overlaps = (dx*dx + dz*dz) <= (aggroRadius * aggroRadius);
```

Use `halfExtents = {radius, generous_height, radius}` for the `queryPolygons` AABB to ensure the bounding box fully contains the circle. The **5% margin** (`AGGRO_MARGIN = 1.05f`) in the recommended code accounts for the player's own collision radius and float imprecision in NPC position updates.

## Every edge case and how to handle it

**Start position inside a danger zone.** The dual-filter approach handles this automatically. The permissive `findPolyFilter` always finds the start polygon. The `pathFilter` with `areaCost[63]=50` penalizes but does not block traversal, so A* begins at the start polygon and immediately seeks to exit the danger zone. The resulting path starts inside danger territory and routes outward — exactly the correct behavior for a bot that already aggroed a mob.

**End position inside a danger zone.** Same mechanism. The pathfinder routes to the destination through danger if necessary. If the caller wants to avoid entering the destination's danger zone, it should move the endpoint to the nearest safe polygon before calling `findPath`. Use `closestPointOnPoly` on nearby non-danger polygons to find a safe alternative endpoint.

**All paths go through danger zones.** Area cost marking guarantees a path is always found (unlike flag exclusion). The path simply traverses the danger zone at high cost. The `throughDanger` check in the result lets the caller decide the response: engage in combat, wait for NPC patrol to move, or attempt the path anyway. This graceful degradation is the primary advantage over flag exclusion.

**Partial paths (`DT_PARTIAL_RESULT`).** Check the status flags from `findPath`. If partial, the path ends at the most promising node the search found. With danger costs, this is usually just before a danger zone. The bot should walk the partial path and then re-query — the NPC may have moved, opening a new route. Never blindly append the destination to a partial path; the gap between the partial endpoint and destination may cross unwalkable terrain.

```cpp
if (status & DT_PARTIAL_RESULT) {
    // Path ends before reaching destination
    if (status & DT_OUT_OF_NODES) {
        // Node pool exhausted — consider increasing maxNodes
        // Or: the search space is simply too large for current settings
    }
    // Walk partial path, re-query at endpoint
}
```

**Multiple overlapping danger zones.** A polygon inside two overlapping zones gets `setPolyArea(AREA_DANGER)` called twice. The `PolyAreaGuard` handles this correctly because it checks `origArea != AREA_DANGER` before saving — the second call is a no-op on an already-marked polygon. No double-restoration occurs.

**NPC positions updating during pathfinding.** Since `setPolyArea` modifies the navmesh in-place and `findPath` reads area types during expansion, concurrent updates are unsafe. The recommended approach runs the entire mark-path-restore sequence **synchronously on a single thread**. The ~500μs total duration makes this practical even at high pathfinding frequency. If you must update NPC positions concurrently, maintain a snapshot of positions at the start of each pathfinding cycle and use only that snapshot.

**Polygon area ID conflicts.** TrinityCore uses areas 8–11 for ground/water/steep/magma. Area 63 is safe to use as the danger designation. However, verify that the game's expansion or custom patches haven't claimed additional area IDs. Check `MMapDefines.h` in your TrinityCore version. If area 63 is taken, any ID in the 12–62 range works identically.

## Performance analysis and caching strategy

The entire pathfinding cycle breaks down into three phases with measurable costs on a **3×3 tile grid (~500 polygons total, 30 NPCs)**:

| Phase | Operations | Estimated time |
|---|---|---|
| Mark danger | 30× `queryPolygons` + 30×~15 `closestPointOnPoly` + ~450 `setPolyArea` | ~150μs |
| Pathfind | 2× `findNearestPoly` + 1× `findPath` + 1× `findStraightPath` | ~300μs |
| Restore | ~450× `setPolyArea` | ~25μs |
| **Total** | | **~475μs** |

At one pathfinding call every 2 seconds, CPU utilization is **0.024%**. This is negligible even on a 32-bit game client sharing CPU time with rendering and game logic.

**Caching danger polygons** between calls provides the biggest optimization. Most NPCs are stationary or move slowly. Maintain a per-NPC cache of affected polygon refs and dirty-flag each NPC when it moves more than **2 units** (roughly half a navmesh polygon width). On each pathfinding cycle, only recompute `queryPolygons` + `closestPointOnPoly` for dirty NPCs. With 30 NPCs and typical patrol behavior, expect **5–10 dirty per cycle**, reducing the marking phase to ~50μs.

```cpp
struct CachedNPC {
    float lastPos[3];
    float aggroRadius;
    std::vector<dtPolyRef> dangerPolys;
    bool dirty = true;
    
    void updatePosition(const float* newPos) {
        float dx = newPos[0] - lastPos[0];
        float dz = newPos[2] - lastPos[2];
        if (dx*dx + dz*dz > 4.0f) {  // Moved > 2 units
            dtVcopy(lastPos, newPos);
            dirty = true;
        }
    }
};
```

**3×3 tile loading is sufficient.** Each TrinityCore tile covers **533 yards**. A 3×3 grid provides ~1600 yards of navigation range in each direction — far beyond typical aggro detection range (30–50 yards) and practical pathfinding distances for a bot. Loading more tiles increases memory (roughly **1MB per tile**) without meaningful pathfinding benefit.

**`maxNodes` sizing**: For a 3×3 grid with ~500 polygons and moderate danger zones, `maxNodes = 2048` provides ample headroom. If you observe `DT_OUT_OF_NODES` in logs, double to 4096. The memory cost is small: each node is ~36 bytes, so 2048 nodes ≈ 72KB.

## Known pitfalls when working with dynamic area costs

**Never use `areaCost < 1.0`.** Detour's source code comments explicitly warn that costs below 1.0 break pathfinding. The A* heuristic (`H_SCALE = 0.999f`) assumes edge costs are at least `1.0 × distance`. Sub-unit costs make the heuristic inadmissible, causing A* to miss optimal paths or expand nodes in wrong order.

**Always restore polygon areas.** A crash between `setPolyArea` and restoration permanently corrupts the in-memory navmesh. The RAII `PolyAreaGuard` pattern ensures restoration even on exceptions. If the DLL can be forcibly unloaded, consider using `dtNavMesh::storeTileState` / `restoreTileState` for batch tile-level backup as a secondary safety net.

**`setPolyArea` modifies the shared `dtNavMesh` in-place.** If the game client also runs pathfinding queries on the same `dtNavMesh` pointer (unlikely for an injected DLL with its own navmesh copy, but possible), concurrent reads during your writes produce undefined behavior. The safe pattern is: all navmesh reads and writes on a single thread, or protect with a read-write lock.

**Polygon granularity limits precision.** Detour operates at polygon level — a polygon partially inside a danger zone gets the full danger cost. In areas with large, sparse polygons (open terrain), the effective danger zone may be significantly larger than the actual aggro radius. In dense areas (cities, dungeons), granularity is much finer. There's no practical workaround within Detour's cost model; this is an inherent limitation of polygon-level cost assignment.

**`findPath` path length limit.** The `maxPath` parameter caps the number of polygon refs returned. For long routes through many small polygons, the path may be truncated. The default in many implementations is 256. For cross-zone bot navigation, use 512 or higher. Truncation manifests as a shortened path without `DT_PARTIAL_RESULT` flag — always compare the last polygon in the path against `endRef`.

## Conclusion

The area cost marking approach with dual filters solves NPC avoidance within Detour's non-virtual filter constraint with zero architectural compromises. The critical design decisions are: **area cost of 50.0** (high enough to force detours of up to 50× direct distance, low enough to preserve float precision and A* efficiency), **`closestPointOnPoly` for circle filtering** (exact polygon-circle intersection via one API call), and **permissive filter for polygon lookup** (eliminates the `findNearestPoly` failure mode that kills flag exclusion). The `throughDanger` boolean in the result gives callers explicit control over engage-vs-wait decisions when avoidance is impossible. With the caching layer and dirty flags, the system handles 30 NPCs at sub-millisecond cost per cycle — invisible in a 2-second pathfinding interval.