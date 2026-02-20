<img src="https://r2cdn.perplexity.ai/pplx-full-logo-primary-dark%402x.png" style="height:64px;margin-right:32px"/>

# Game Bot Pathfinding: NPC Avoidance, Long-Distance Navigation, and Movement Algorithms

Context

I'm building a bot for a 32-bit MMORPG client (WoW 3.3.5a, build 12340). The bot is an injected DLL that uses
Recast/Detour navigation mesh (TrinityCore-generated mmaps). Current setup:

- Detour navmesh with 32-bit dtPolyRef (not 64-bit)
- Pre-generated TrinityCore .mmap/.mmtile files (cannot rebuild tiles)
- dtQueryFilter is NOT virtual (cannot subclass passFilter/getCost)
- Currently loading a 3×3 tile grid around the player (~1600 yards range)
- Each tile covers 533.33 yards (map size 64×64 tiles)
- maxNodes = 65535 (dtNodeIndex = unsigned short)
- Movement via ClickToMove (CTM) — sends player to a world coordinate
- Have access to all nearby NPC positions, levels, hostility, facing
- Aggro radius formula: 20 + (creatureLevel - playerLevel), clamped [5, 45]

Questions

1. Hostile NPC Avoidance with Detour Navmesh

a) What is the definitive correct implementation of dynamic obstacle avoidance using Detour's setPolyArea +
setAreaCost approach? Specifically:

- What is the optimal areaCost value for danger zones and why? (Some sources say 50.0, others say higher — what
are the float precision and A* behavioral tradeoffs at different cost values like 10, 50, 100, 500, 1000?)
- How much safety margin should be added to aggro radius? Multiplicative (1.05×, 1.10×, 1.15×) vs additive (+2yd,
+5yd, +8yd)? What factors determine the right choice?
- How should the throughDanger flag be computed? Should first/last polys be excluded from the check (player may
already be in danger zone)?

b) How should the bot behave when the A* path goes through danger zones (no safe route exists)?

- Should it always follow the A*-optimized path (minimum danger exposure)?
- When should it switch to combat vs. waiting vs. attempting to run through?
- How do professional WoW bots (HonorBuddy, WRobot, etc.) handle this decision?

c) How do modern game AI systems handle dynamic NPC avoidance at the navigation level? Are there approaches beyond
Detour's area cost system that work well for pre-generated navmeshes?

d) What about NPC patrol paths? If an NPC is patrolling, its danger zone is a moving circle. What algorithms
handle moving obstacles in navmesh pathfinding? How often should the danger zones be re-evaluated — every
pathfinding call, or can the marking be cached and only refreshed when NPCs move significantly?

2. Long-Distance Navigation (Beyond 3×3 Tiles)

a) How do game bots navigate across an entire continent (thousands of yards)? The bot needs to walk from, say,
Stormwind to Ironforge — that's hundreds of tiles. How is this typically done?

- Tile streaming strategies: How to load/unload tiles dynamically as the player moves? What's the optimal tile
loading radius — 3×3, 5×5, corridor-based?
- Hierarchical pathfinding (HPA)*: How does it work with Detour/Recast navmesh? Can you precompute a high-level
graph of tile-to-tile connections and then do local pathfinding within each tile?
- Waypoint graph overlay: Is it better to use a manually-created waypoint graph for long-distance navigation and
only use the navmesh for local obstacle avoidance?
- Corridor/funnel-based loading: Instead of a square grid, load only tiles along the planned route?

b) What is the memory cost and performance impact of loading more tiles? With TrinityCore mmaps:

- Average tile size in memory
- How many tiles can a 32-bit process realistically hold (address space limited to ~2GB)
- Can tiles be loaded/unloaded on-the-fly without invalidating active dtNavMeshQuery state?
- How does findPath performance scale with the number of loaded tiles?

c) How does TrinityCore's own PathGenerator handle long-distance paths (>1600 yards)? It uses the same mmaps —
what's their tile loading strategy?

d) What algorithms are used for cross-zone and cross-continent navigation? How to handle zone transitions,
portals, flights, ships, teleporters in the navigation graph?

3. Optimal Movement Algorithms

a) What is the best approach for smooth waypoint following with ClickToMove (CTM)?

- CTM gives direct-line movement to a point. How to handle curved paths (multiple waypoints)?
- What arrival threshold works best? (Too small = oscillation, too large = cutting corners into obstacles)
- How should the bot handle stuck detection and recovery? Best practices for detecting stuck (moved < threshold in
N seconds) and recovering (jump, backtrack, repath)?

b) How do advanced bots implement path smoothing? Detour's findStraightPath already does string-pulling, but are
there additional smoothing passes that help with CTM movement?

c) What's the optimal re-pathing frequency? Constant re-pathing causes oscillation, too infrequent means stale
paths. What are the best practices?

- Different frequencies for different situations (combat, traveling, following)?
- Cooldown-based vs event-based re-pathing triggers?
- How to prevent path oscillation when danger zones are close to the path?

4. Advanced Pathfinding Algorithms

a) Beyond basic A* on navmesh, what algorithms are used in commercial game bots?

- Theta / Any-angle pathfinding*: Worth implementing on Detour navmesh?
- D Lite / Incremental replanning*: For efficient re-pathing when obstacles move?
- Potential fields / Influence maps: Can these be combined with navmesh pathfinding for NPC avoidance?
- RRT (Rapidly-exploring Random Trees): Useful for finding paths in highly constrained spaces?
- Flow fields: How are they used in game AI and could they help with area-level navigation?

b) How do high-end WoW bots handle pathfinding? What architecture do they use?

- Do they use TrinityCore mmaps or custom navigation data?
- How do they handle multi-floor areas (caves, buildings)?
- What about water, swimming, flying paths?

c) How can navmesh pathfinding be combined with a higher-level planning system? For example:

- "Go to X" should produce: walk to boat → take boat → walk to destination
- How is this modeled? State machines? Goal-oriented action planning (GOAP)? Behavior trees?

5. Detour-Specific Technical Details

a) dtNavMeshQuery::findPath internals:

- How exactly does the node pool work? When does DT_OUT_OF_NODES occur and what's the best recovery strategy?
- What is the relationship between maxNodes, number of loaded tiles, and path length?
- Is there a way to estimate the required maxNodes for a given navigation scenario?

b) Tile management API:

- Can dtNavMesh::removeTile / addTile be called while a dtNavMeshQuery is in use?
- What's the safe procedure for tile streaming (load ahead, unload behind)?
- Does removing a tile invalidate poly refs from that tile in an in-progress query?

c) Are there Detour configuration options or patches that improve pathfinding quality for bot-like use cases? For
example:

- Custom heuristics (overriding H_SCALE)?
- Bidirectional A*?
- Any-angle path optimization post-processing?

6. Anti-Detection Considerations for Movement

a) What movement patterns make bots detectable by Warden or statistical analysis?

- Perfectly straight paths between waypoints
- Constant speed, no acceleration/deceleration
- Instant direction changes
- Unnaturally efficient obstacle avoidance
- How do advanced bots add realistic movement noise?

b) How to make pathfinding behavior appear human-like?

- Random minor deviations from the optimal path
- Occasional stops, speed variations
- Natural-looking obstacle avoidance (not pixel-perfect)

Please provide detailed, actionable implementation guidance with code examples where relevant. Focus on approaches
that work with Detour/Recast and pre-generated TrinityCore navmesh data. Include references to real
implementations, papers, and open-source projects where available.

For a WoW 3.3.5 bot on TrinityCore mmaps you want: area‑cost–based “danger overlays” on top of Detour, a hierarchical long‑range router with tile streaming, CTM‑aware smoothing and stuck handling, and a thin “humanization” layer that perturbs otherwise optimal movement.

Below is a practical design, focused on what you can implement with 32‑bit Detour + Trinity mmaps and a non‑virtual `dtQueryFilter`.

***

## 1. Hostile NPC avoidance on Detour

### 1a. Area costs and danger zones

**Data model**

Since you can’t subclass `dtQueryFilter`, use areas and flags:

- Define areas: `GROUND`, `DANGER`, maybe `VERY_DANGER`.
- Use `dtNavMesh::setPolyArea`/`setPolyFlags` to tag polygons around hostile NPCs at runtime.[^1]
- Use `dtQueryFilter::setAreaCost(area, cost)` to make A* avoid them.[^2]

Because you can’t rebuild mmaps, maintain a side table:

```cpp
struct PolyBackup { dtPolyRef ref; unsigned char origArea; unsigned short origFlags; };

std::vector<PolyBackup> g_dangerBackup;

void markDanger(dtNavMesh* nav, const dtPolyRef* polys, int count) {
    g_dangerBackup.clear();
    for (int i=0;i<count;++i) {
        const dtPolyRef ref = polys[i];
        const dtMeshTile* tile; const dtPoly* poly;
        if (dtStatusFailed(nav->getTileAndPolyByRef(ref, &tile, &poly))) continue;
        PolyBackup b{ ref, poly->getArea(), poly->flags };
        g_dangerBackup.push_back(b);
        nav->setPolyArea(ref, AREA_DANGER);
        nav->setPolyFlags(ref, poly->flags | FLAG_DANGER);
    }
}

void restoreDanger(dtNavMesh* nav) {
    for (auto& b : g_dangerBackup) {
        nav->setPolyArea(b.ref, b.origArea);
        nav->setPolyFlags(b.ref, b.origFlags);
    }
    g_dangerBackup.clear();
}
```

Detour’s path cost is basically geometric distance multiplied by area costs (with some averaging across the edge), so only the *ratio* between costs matters.[^2]

**Choosing area costs**

Heuristics that work well in practice:

- Base: `GROUND = 1.0`.
- Moderate danger: `DANGER = 10–30` → bot will make a sizeable detour but still use a short danger shortcut if the alternative is huge.
- Extreme danger (e.g. elite packs): `VERY_DANGER = 50–100`.

There is no float‑precision issue for values in the 1–100 range on WoW‑scale maps; Detour paths are routinely used on much larger worlds in engines like Unreal and game servers using Recast. Values in the thousands still work, they just make g‑costs large without changing ordering.[^3][^2]

**Safety margin around aggro radius**

Aggro radius: $R = \text{aggroBase} + \text{levelDelta}$, clamped $[5,45]$.

Add both:

- Multiplicative to account for latency/path error: `R1 = R * 1.10–1.20`.
- Additive padding to account for navigation granularity: `R2 = R1 + 2–5 yd`.

Pick the padding based on:

- Navmesh resolution: coarser mesh → use more padding.
- Your CTM arrival radius (see §3): if you often cut corners within ~2 yd, you want padding ≥ that.

**Marking danger polys**

Use `dtNavMeshQuery::findPolysAroundCircle(startRef, centerPos, radius, filter, outRefs, parentRefs, costs, &count, max)` around each hostile NPC to collect affected polys. Combine all NPCs into one overlay per frame (or per few frames):[^4][^5]

```cpp
void rebuildDangerOverlay(dtNavMesh* nav, dtNavMeshQuery* q,
                          const std::vector<Npc>& hostiles,
                          const dtQueryFilter& filter)
{
    restoreDanger(nav);

    static dtPolyRef tmpRefs[^4096];
    dtPolyRef startRef;
    float npcPos[^3];

    for (auto& npc : hostiles) {
        // choose only hostiles that can aggro (LOS, level diff, etc.)
        float radius = computeAggroRadius(npc) * 1.15f + 3.0f;
        npcPos[^0] = npc.x; npcPos[^1] = npc.y; npcPos[^2] = npc.z;
        q->findNearestPoly(npcPos, extents, &filter, &startRef, 0);
        int count = 0;
        q->findPolysAroundCircle(startRef, npcPos, radius, &filter,
                                 tmpRefs, nullptr, nullptr, &count,
                                 DT_ARRAYSIZE(tmpRefs));
        markDanger(nav, tmpRefs, count);
    }
}
```

This keeps Detour unchanged while giving you a dynamic “heat map” overlay.

**`throughDanger` flag**

A simple helper:

```cpp
bool pathGoesThroughDanger(const dtPolyRef* path, int n,
                           bool ignoreEndpoints)
{
    int i0 = ignoreEndpoints ? 1 : 0;
    int i1 = ignoreEndpoints ? n-1 : n;
    for (int i=i0;i<i1;++i) {
        if (isDangerPoly(path[i])) // from FLAG_DANGER
            return true;
    }
    return false;
}
```

- For travel: usually ignore first poly (you may already be in a bad spot) but *not* last (you want to know if target is inside a camp).
- For combat pulls: often ignore last poly (you *want* to end in danger) but not mid‑path.

***

### 1b. When no safe route exists

Typical policy:

- If there *is* a path with only moderate danger (say only `DANGER`, no `VERY_DANGER`), follow the Detour A* path: this already minimizes time in danger given your area costs.[^2]
- If *all* paths require `VERY_DANGER`:
    - If target is required (quest objective, corpse, instance portal), consider:
        - Try to skirt the edge: temporarily downgrade `VERY_DANGER` to `DANGER` and replan; if the new path touches only 1–2 danger polys, run through.
        - If surrounded (no non‑danger neighbor around target), either:
            - Switch to a combat mode: pull and clear the camp.
            - Or blacklist this target for a cooldown (e.g., 5–15 min) and pick another objective.

High‑end bots typically combine navmesh with user‑editable blackspots and profiles (“don’t path through these hotspots”), and they prefer to clear packs rather than path pixel‑perfectly between tight patrols; similar behavior is discussed in community bots like AmeisenNavigation and WRobot which use TrinityCore mmaps and Detour.[^6][^7]

***

### 1c. Modern dynamic NPC avoidance

Modern systems separate **global pathfinding** from **local avoidance**:

- Global: A* (or variants) on a navmesh or grid.
- Local: continuous steering/velocity‑obstacle solvers around moving agents, e.g. RVO/ORCA, or DetourCrowd’s RVO‑based avoidance.[^8][^9][^2]

Detour itself has DetourCrowd:

- Keeps a *path corridor* per agent (`DetourPathCorridor`) and lets local avoidance push the agent off the exact centerline while periodically “re‑fitting” the corridor to stay optimal.[^10]
- Uses an RVO‑like sampler for neighboring agents and obstacles.[^9]

For your bot, full DetourCrowd is probably overkill, but you can borrow the idea:

- Use Detour only for coarse path (straight path points).
- On top of that, maintain a “corridor index” and perform small lateral offsets when near NPC danger disks, as long as you stay on the same straight‑path segment.

***

### 1d. Patrols / moving danger

Model patrols as moving disks:

- At each danger update tick (e.g., every 0.5–1.0 s while traveling):
    - Use current NPC position along its path.
    - Mark danger polys around its *current* aggro circle as in 1a.
    - Optionally enlarge radius in direction of motion to compensate for prediction error.

Caching:

- Do *not* permanently cache marked polys by NPC ID; just recompute per update tick.
- Reuse temporary arrays; avoid reallocations.
- Rebuild overlay only if an NPC moved more than some threshold since last mark (e.g., >3–5 yd) to save work.

Pathfinding frequency:

- Replan when:
    - Entering a danger shell (`throughDanger` switched from false→true).
    - The target moved significantly (follow mode).
    - A patrol has crossed your path within some time window.
- Otherwise, rely on corridor following rather than constant full re‑path.

Detour’s own path‑corridor code supports updating agent and target positions incrementally to adjust to dynamic changes without full re‑path each frame.[^10]

***

## 2. Long-distance navigation

### 2a. Strategy for continent‑scale paths

Common architecture:

1. **High‑level routing graph**
    - Nodes: hubs (cities, flight masters, boats, zeppelins, dungeon portals, major crossroads).
    - Edges: “can traverse” links with known costs: flight, boat travel time, or “walk along navmesh from A to B”.
    - Run A* on this abstract graph to get a sequence of *segments*: walk → boat → walk → flight etc.
2. **Segment‑level Detour paths**
    - For each walk segment, run navmesh A* between the two hubs in world coordinates.
    - For each transport, hand off to special logic (boarding, waiting, disembarking).

**Tile streaming radius**

With a tiled navmesh (`dtNavMesh::addTile/removeTile`), you can stream only those tiles around the agent and near-term goal.[^1][^2]

Patterns that work well:

- While traveling:
    - Load a 5×5 or 7×7 tile window around the *player* (your current correction radius).
    - Additionally, ensure tiles along the next N hundred yards toward the straight‑line target are loaded (corridor loading).
- When planning *very* long segments:
    - Either:
        - Plan on a *reduced* mesh (e.g., pre‑baked portal graph between tiles).
        - Or plan in stages: from current pos to an intermediate waypoint ~1–2k yards away, walk it, then replan from there.

Several open‑source bots built on TrinityCore mmaps (e.g., AmeisenNavigation) run Detour as a standalone server and support continent‑scale “smooth pathfinding” by combining navmesh pathing with higher‑level logic and tile streaming.[^6]

**Waypoint overlay**

For very long navigation, it’s reasonable to have a sparse waypoint/road graph (manually curated or auto‑extracted) and:

- Use high‑level A* on that graph for continent travel.
- Use navmesh only to go:
    - From your current pos to the nearest waypoint.
    - Between waypoints when they’re nearby.
    - From final waypoint to the concrete destination.

This reduces Detour work and gives you nice “follow roads” behavior that looks human.

***

### 2b. Memory, 32‑bit limits, tile counts

Recast navmeshes are designed for tile streaming in large open worlds, with `addTile`/`removeTile` primitives specifically for that purpose.[^1][^2]

Practical guidelines for a 32‑bit WoW client bot:

- Keep a fixed cap on loaded tiles (e.g., 5×5 or 7×7 around player, plus a small strip along current detour path).
- Maintain your own LRU/region set and unload tiles that are far behind.
- Detour’s pathfinding cost is essentially proportional to the number of visited polys; having unrelated tiles loaded doesn’t hurt too much, but removing obviously unreachable tiles helps cache and memory.

`maxNodes` in `dtNavMeshQuery::init(nav, maxNodes)` controls how many search nodes A* can allocate in its node pool. More tiles → more reachable polys → potentially more nodes. With a 3×3 or 5×5 window, 65k nodes is usually enough unless the mesh is extremely dense.[^11][^12][^4]

Tiles can be loaded and unloaded at runtime using `addTile` and `removeTile`; this is the mechanism used by engines and servers to manage large maps.[^2][^1]

***

### 2c. TrinityCore PathGenerator specifics

TrinityCore’s `PathGenerator` uses the same Recast/Detour code and mmaps to compute server‑side paths for creatures and movement, as seen in `PathGenerator.cpp` in the core.[^13]

Known from issues and docs:

- It uses Detour’s poly path + straight path pipeline (`findPath`, `findStraightPath`).[^13][^2]
- Path weirdness is often discussed in relation to Detour parameters and mmaps quality; improvements focus on mesh detail and Detour configuration, not a fundamentally different algorithm.[^14][^15][^16]

For long paths, the server can load any tiles it needs (with more RAM than a 32‑bit client bot), so your bot doesn’t need to mimic its exact streaming; but looking at `PathGenerator.cpp` gives a reference for reasonable parameters (search radii, step sizes, etc.).[^13]

***

### 2d. Cross‑zone / cross‑continent, portals and transports

Treat “world navigation” as a graph problem:

- Nodes:
    - World positions with semantics: city gates, portals, instance entrances, zeppelins/boats, flight masters.
- Edges:
    - **Walk**: navmesh segment between two nodes on same map/phase.
    - **Teleport / Portal**: zero or fixed‑cost edges linking entry→exit nodes.
    - **Transport**: edges with:
        - Conditions (boat present, schedule, phase).
        - A scripted sequence: walk onto boat → wait → teleport to target map / coordinates.

Algorithm:

1. Run A* on this world graph based on *estimated travel time*.
2. This yields a plan like: “Walk to Menethil dock → boat to Theramore → walk to target”.
3. For each “walk” edge, do a Detour path on the correct map ID.

This is how serious bots and leveling frameworks structure “go to X” navigation: Detour is just the local solver.

***

## 3. Movement and CTM algorithms

### 3a. Waypoint following via CTM

CTM is point‑to‑point, so you need a *follower* on top of Detour’s straight path:

1. Path generation:
    - Use `findPath` → poly path → `findStraightPath` → straight points.[^5][^4]
    - Optionally decimate (see §3b).
2. Follower loop:
```cpp
struct PathFollower {
    std::vector<Vec3> pts;
    size_t idx = 0;
    float arrivalRadius = 2.0f; // in yards
};

void updateFollower(PathFollower& f, const Vec3& playerPos) {
    if (f.idx >= f.pts.size()) return;

    while (f.idx < f.pts.size()) {
        float dist = distance2D(playerPos, f.pts[f.idx]);
        if (dist <= f.arrivalRadius) {
            ++f.idx;
            continue;
        }
        sendCTM(f.pts[f.idx]); // write CTM struct in client
        break;
    }
}
```

- **Arrival radius**: 1.5–3 yd works well on WoW; smaller tends to cause oscillations near obstacles, larger cuts corners too aggressively.
- If the next waypoint is almost collinear with the following one, skip it (see smoothing).

**Stuck detection and recovery**

Typical approach:

```cpp
Vec3 lastPos;
double lastMoveTime;
bool stuck = false;

void checkStuck(const Vec3& curPos, double now) {
    float moved = distance2D(curPos, lastPos);
    if (moved > 1.0f) { // moved at least 1 yd
        lastPos = curPos;
        lastMoveTime = now;
        stuck = false;
        return;
    }
    if (now - lastMoveTime > 3.0) { // 3 seconds without progress
        stuck = true;
    }
}

void recoverStuck() {
    // Simple heuristics:
    // 1. Stop CTM, jump.
    // 2. Take a small random back/strafe step.
    // 3. Repath from current pos to current segment goal.
}
```

Community WoW botters often use rules like “if turn angle > 90°, stop moving forward before turning” to avoid snagging on corners, which meshes well with CTM‑only movement and helps with stuck prevention.[^17]

***

### 3b. Path smoothing beyond `findStraightPath`

Detour’s `findStraightPath` is already a funnel/string‑pulling pass that produces the locally straightest path inside the poly corridor.[^10][^2]

Additional smoothing passes that help CTM:

- **Point decimation**
    - Drop intermediate points where the angle change is below some threshold, e.g. $\Delta\theta < 5^\circ$.
    - Keep turn points and portal exits.
- **Curve fitting (optional)**
    - Some bots (e.g. AmeisenNavigation) then fit Catmull‑Rom or Bezier curves through these waypoints to output denser, smooth micro‑points, but with CTM you usually don’t need that; CTM already handles straight‑line motion between points.[^6]
- **Corner padding**
    - When you detect a sharp turn, insert an intermediate waypoint slightly before the corner along the incoming segment, so you decelerate/turn more naturally instead of pivoting on the exact vertex.

***

### 3c. Re‑pathing frequency and oscillation

Avoid constant replanning:

- Travel mode:
    - Replan on:
        - Target change.
        - Teleport/phase change.
        - Stuck event.
        - Detection that path goes through newly created danger (patrol walked into path).
    - Otherwise, reuse old path for seconds at a time (5–15 s), just advancing waypoints.
- Combat / follow:
    - Use shorter re‑path intervals (e.g. 0.5–1.5 s), because target moves a lot.
    - Or event‑based: replan only when distance to target > X or LOS broken.

To avoid oscillations when danger zones are near:

- Add **hysteresis**:
    - Remember last “side” chosen around an obstacle; don’t flip unless a big benefit appears.
    - Add a small cost penalty to polygons that just flipped from safe→danger to discourage immediate backtracking.
- Rate limit:
    - After a re‑path, suppress further re‑paths for at least N seconds unless stuck or target changed drastically.

***

## 4. Advanced pathfinding options

### 4a. Algorithms beyond vanilla A*

On a Detour navmesh:

- **Any‑angle (Theta\*)**
    - On navmeshes, Detour’s funnel already gives you the “any‑angle” path inside the poly corridor; Theta\* mostly helps grids.
    - Additional benefit is usually small compared to funnel, so not worth complexity here.
- **Incremental replanning (D*, D* Lite)**
    - You can emulate much of this with Detour’s *sliced* pathfinding:
        - `initSlicedFindPath` / `updateSlicedFindPath` / `finalizeSlicedFindPathPartial` let you update paths over multiple frames and get partial paths.[^4][^5]
    - For a single player bot, full D* is probably overkill; dynamic overlays + occasional replans suffice.
- **Potential fields / influence maps**
    - Quite useful *above* Detour:
        - Maintain a coarse grid (or cell graph) with accumulated danger influence (NPCs, players, PvP hotspots).
        - Use this to:
            - Bias your choice of “macro waypoints”.
            - Adjust area costs before each Detour call (e.g., higher cost in grid cells with high influence).
- **Flow fields**
    - Great for many agents going to similar goals (RTS), but heavy for a single player bot.
    - You could precompute “flows” along common corridors (e.g., road directions), but the benefit over simple waypoints is modest.
- **RRT**
    - Rare in character navigation for games; more common for high‑DOF robotics.
    - Unnecessary given Detour’s navmesh and A* is extremely fast on WoW‑scale maps.

***

### 4b. How high‑end WoW bots tend to do it

From available public projects and discussions:

- Many serious bots use TrinityCore mmaps/Recast+Detour either directly or through a nav server (e.g., AmeisenNavigation).[^18][^19][^6]
- Multi‑floor areas (caves, buildings) are handled *implicitly* by navmesh: polys differ in height, and Recast’s voxelization already separates floors.[^3][^2]
- Water/swimming/flying:
    - Some frameworks generate separate meshes:
        - Ground mesh, water volume mesh, flying mesh.
    - Bot logic then chooses which navmesh to query based on movement mode; AmeisenNavigation, for example, mentions plans for flying path generation.[^6]

***

### 4c. Combining navmesh with higher‑level planning

A robust structure:

- **Top‑level planner**
    - GOAP or simple goal stack: “Be in zone X”, “Complete quest Y”, “Grind to level Z”.
- **Travel planner**
    - Given `(currentPos, targetPos)`, returns a *plan*:
        - Steps: walk segment, mount, flight, portal, boat, etc.
    - This is where the world graph lives (see §2d).
- **Local navigator (Detour)**
    - For each “walk” step, returns a poly/straight path.
- **Executor / behavior tree**
    - A BT or state machine runs:
        - `MoveTo(destination)` using CTM + path follower.
        - `UseTransport`, `InteractNpc`, etc.

For example, “Go to X”:

1. Travel planner: Stormwind → (walk) → harbor → (boat) → Borean Tundra → (walk) → X.
2. For each walk segment, call Detour; for each boat step, run a boat‑handling behavior.

***

## 5. Detour technical details

### 5a. Node pool, `maxNodes`, and `DT_OUT_OF_NODES`

`dtNavMeshQuery` holds a `dtNodePool* m_nodePool` with capacity `maxNodes`, set at `init(nav, maxNodes)`.[^12][^11][^4]

- Each visited poly along the search frontier consumes a node.
- `DT_OUT_OF_NODES` occurs when A* tries to push more nodes than `maxNodes` permits; the search aborts, and you often get only a partial path (if using sliced API).[^5][^4]

Relationship:

- More loaded tiles → more reachable polys.
- More open space / fewer obstacles → more candidate expansions.
- Long straight corridor with few branches: fewer nodes.
- Wide open field with many neighbors per poly: many nodes.

Estimating `maxNodes`:

- Rough rule: `maxNodes >= k * expectedVisitedPolys`.
- You can instrument an offline run:
    - Walk around with an instrumented nav server (like AmeisenNavigation) tracking max frontier size per region, then set `maxNodes` accordingly for the bot.[^6]

Recovery strategies:

- If you see `DT_OUT_OF_NODES`:
    - Re‑init `dtNavMeshQuery` with a larger `maxNodes` for large‑scale travel.
    - Or fall back to a shorter search radius (e.g., plan to an intermediate waypoint instead of full target).

***

### 5b. Tile management and `dtNavMeshQuery`

`dtNavMesh` provides:

- `addTile(data, dataSize, flags, lastRef, &result)` – add tile.[^1]
- `removeTile(tileRef, &data, &dataSize)` – remove tile (optionally returning memory so you can free).[^1]
- `calcTileLoc(pos, &tx, &ty)` – world→tile indices, useful for streaming.[^1]

Safe usage pattern in a single‑threaded bot:

- Do not call `addTile/removeTile` while a `findPath` is in progress.
- It is safe to reuse a single `dtNavMeshQuery` instance as long as you only call its methods after tile operations have finished; constant methods are documented as usable across clients with no side effects.[^12][^5]
- Removing a tile invalidates any `dtPolyRef` that points into it; make sure:
    - Your active path does not contain polys from tiles you unload.
    - Or, simply: when you unload tiles, discard current path and re‑plan.

So for streaming:

1. Compute new desired tile window around player.
2. Remove tiles outside window; add new tiles inside.
3. Invalidate current path and query state.
4. Re‑issue `findPath` / `findStraightPath` as needed.

***

### 5c. Detour tuning / patches for bots

Some commonly useful tweaks (many seen or discussed in Recast/Detour and engine integrations).[^15][^16][^14][^2]

- **Heuristic scale (`H_SCALE`)**
    - A slightly *over*‑admissible heuristic (e.g., 1.05–1.2× straight‑line distance) can reduce node expansions and biased exploration without affecting path quality too much.
    - Requires patching Detour, but is straightforward.
- **Bidirectional A***
    - Not built into Detour, but you can layer it above (two `dtNavMeshQuery` instances expanding from start and end) at the cost of complexity.
    - Gains are modest on well‑behaved navmeshes and not strictly necessary.
- **Post‑processing any‑angle step**
    - After `findStraightPath`, you can raycast between non‑adjacent waypoints in small windows to see if Detour’s funnel was overly conservative; if LOS on navmesh holds, skip intermediate points.
    - This is cheap and can slightly reduce waypoints for CTM.

Several TrinityCore issues document experiments with Recast parameters and Detour behavior to get better NPC paths, which can inform your bot’s expectations (e.g., occasional weird paths are normal and should be handled gracefully).[^16][^14][^15]

***

## 6. Anti‑detection movement behavior

### 6a. What looks bottish

Detectable patterns (from botting and game‑AI discussions):

- Perfectly straight, optimal paths with no micro‑corrections.
- Constant speed, no acceleration or overshoot.
- Instant 90–180° turns without drift.
- Zero latency on decisions (no hesitation, perfect timing).
- Extremely consistent key intervals and mouse/CTM timings.[^20][^21][^22]

Modern detectors often use ML models on trajectories and input timing; research on human vs machine mouse trajectories shows that humans have “ineffective” micro‑movements, variable speed, and noisy curves.[^21][^22][^20]

***

### 6b. Making paths look human

Layer a **noise model** on top of your navmesh path and CTM:

1. **Spatial noise**
    - For each straight path segment, offset intermediate waypoints by a small random vector perpendicular to the path (e.g., ±0.3–0.8 yd) while staying inside the same navmesh polys (optional raycast check).
    - Occasionally step a bit to the side when approaching corners instead of hugging the wall.
2. **Temporal noise**
    - Randomize CTM issue intervals slightly (e.g., ±50–150 ms).
    - Add occasional brief pauses:
        - 0.3–1.0 s idle when nothing urgent is happening.
    - Modulate movement speed via WoW’s existing systems:
        - Small delays between CTM updates mimic human reaction rather than constant re‑targeting.
3. **Turn and camera behavior**
    - Avoid instant perfect facing changes; let the client’s natural turning curve handle it by not spamming CTM facing updates too fast.
    - Occasionally adjust camera or do small “look around” spins while idle (if safe).
4. **Imperfect obstacle avoidance**
    - Don’t always take the absolute shortest path between two close waypoints.
    - Sometimes allow a mild brush against objects or a small overshoot, then correct.

Discussions on human‑like MMORPG bots and mouse movement show that including these micro‑imperfections significantly helps mimic human trajectories.[^22][^7][^20][^21]

***

If you want, next step can be to sketch a concrete module layout (navmesh server vs in‑process, tile manager, danger overlay manager, CTM follower, and “humanizer”) with more detailed C++ pseudocode tailored to your current 3×3‑tile implementation and how to extend it safely.
<span style="display:none">[^23][^24][^25][^26][^27][^28][^29][^30]</span>

<div align="center">⁂</div>

[^1]: https://recastnav.com/classdtNavMesh.html

[^2]: https://recastnav.com

[^3]: https://github.com/TrinityCore/TrinityCore/blob/master/dep/recastnavigation/README.md

[^4]: https://www.vaikene.ee/planeshift/api/classdtNavMeshQuery.html

[^5]: https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Runtime/Navmesh/Detour/dtNavMeshQuery

[^6]: https://github.com/Jnnshschl/AmeisenNavigation

[^7]: https://wrobot.eu/forums/topic/8379-movement-smoother/

[^8]: https://dev.epicgames.com/documentation/en-us/unreal-engine/using-avoidance-with-the-navigation-system-in-unreal-engine

[^9]: https://github.com/recastnavigation/recastnavigation/discussions/711

[^10]: https://github.com/recastnavigation/recastnavigation/blob/master/DetourCrowd/Source/DetourPathCorridor.cpp

[^11]: https://atoms.toolchefs.com/docs/sdk/class_atoms_utils_1_1dt_nav_mesh_query.html

[^12]: https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Runtime/Navmesh/dtNavMeshQuery

[^13]: https://github.com/TrinityCore/TrinityCore/blob/master/src/server/game/Movement/PathGenerator.cpp

[^14]: https://github.com/TrinityCore/TrinityCore/issues/17878

[^15]: https://github.com/TrinityCore/TrinityCore/issues/22470

[^16]: https://github.com/TrinityCore/TrinityCore/issues/23028

[^17]: https://www.ownedcore.com/forums/world-of-warcraft/world-of-warcraft-bots-programs/wow-memory-editing/312365-bot-movement-other-than-ctm-2.html

[^18]: https://www.ownedcore.com/forums/world-of-warcraft/world-of-warcraft-bots-programs/wow-memory-editing/300753-what-nav-do-you-use.html

[^19]: https://www.ownedcore.com/forums/world-of-warcraft/world-of-warcraft-bots-programs/wow-bots-questions-requests/900641-recast-detour-navmesh-cmangos-wheres-my-cactus-missing-objects-nav-mesh.html

[^20]: https://www.reddit.com/r/gameai/comments/ao37fq/replicating_humanlike_behavior_in_a_mmorpg_bot/

[^21]: https://arxiv.org/html/2410.18233v1

[^22]: https://osbot.org/forum/topic/86494-implementation-of-human-like-mouse-movement/

[^23]: https://github.com/RGreenlees/DTBotTemplate

[^24]: https://support.aurasoft-skyline.co.uk/index.php?do=details\&task_id=85

[^25]: https://github.com/TrinityCore/TrinityCore/issues/7592

[^26]: https://www.leadwerks.com/community/topic/3802-using-detour-to-handle-dynamic-obstactles/

[^27]: https://www.youtube.com/watch?v=ucNkBiIo6o4

[^28]: https://drewkestell.us/Article/6/Chapter/10

[^29]: https://www.youtube.com/watch?v=WKso-wu68v8

[^30]: https://www.reddit.com/r/unrealengine/comments/9jdb8u/detour_crowd_ai_controller_and_rvo_avoidance/

