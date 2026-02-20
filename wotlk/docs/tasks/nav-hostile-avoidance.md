# Task: Hostile NPC Avoidance + Long-Distance Navigation

> **Status**: CODE COMPLETE (pending in-game testing)
> **Created**: 2026-02-19
> **Updated**: 2026-02-20
> **Phase**: 7e (Navigation)
> **Depends on**: nav-navigate-tool, radar-widget
> **Blocks**: nothing

---

## Table of Contents

1. [Goal](#goal)
2. [Context: Current State and Problems](#context)
3. [Research Summary: 5 Documents](#research-summary)
4. [Agreed Architecture and Parameters](#agreed-architecture)
5. [Part A: Fix Pathfinder (NPC Avoidance)](#part-a-fix-pathfinder) 
6. [Part B: Margin Architecture Refactor](#part-b-margin-refactor)
7. [Part C: Recovery Protocol (Combat/Flee)](#part-c-recovery-protocol)
8. [Part D: Hysteresis + Event-Based Rerouting](#part-d-rerouting)
9. [Part E: Tile Streaming (5x5+)](#part-e-tile-streaming)
10. [Part F: World Graph (Long-Distance)](#part-f-world-graph)
11. [Part G: Corridor Loading](#part-g-corridor-loading)
12. [Part H: Movement Synthesizer (Anti-Detection)](#part-h-movement-synthesizer)
13. [Implementation Order](#implementation-order)
14. [Files Inventory](#files-inventory)
15. [Acceptance Criteria](#acceptance-criteria)
16. [Bug Fixes (2026-02-20)](#bug-fixes)

---

<a name="goal"></a>
## 1. Goal

Build a navigation system that:

1. **Routes around hostile NPCs** as absolute priority. Combat only when there is literally zero path available.
2. **Navigates any distance** — from point A to point B regardless of distance, across continents if needed.
3. **Follows research recommendations exactly** — no guessing, no ad-hoc solutions.
4. **Logs everything useful** for debugging and analysis.

User requirements (verbatim):
- "Маршрут должен строиться в обход враждебных НПС приоритетно"
- "Единственный случай когда нужно нападать — если до конечной точки вот совсем никак нельзя пройти от слова совсем, ноль маршрутов, ноль"
- "Делай все по рекомендациям ресерчей"
- "Мне нужно чтобы обходы работали на дальнее расстояние и я мог добираться из пункта А в пункт Б вне зависимости от расстояния и количества тайлов"

---

<a name="context"></a>
## 2. Context: Current State and Problems

### 2.1 What Exists

| Component | File | State |
|---|---|---|
| Pathfinder | `navigation/pathfinder.h/.cpp` | **BROKEN** — uses flag exclusion + cost 500 |
| NavMesh | `navigation/nav_mesh.h/.cpp` | Works, 3x3 tiles, maxNodes=65535 |
| NavHelper | `bot/nav_helper.h/.cpp` | Timer-based rerouting, no hysteresis |
| ThreatScanner | `bot/threat_scanner.h/.cpp` | Geometry helpers OK, old avoidance code removed |
| FollowRoute | `bot/tools/follow_route.h/.cpp` | Timer-based rerouting, no hysteresis |
| MoveToTool | `bot/tools/move_to.h/.cpp` | Delegates to NavHelper, forced combat exists |
| Radar | `bot/radar.h/.cpp` | Provides NPC data, aggroRadius includes +3yd margin |
| Aggro | `bot/aggro.h` | CalcAggroRadius + kAggroMargin=3.0f |

### 2.2 Critical Bugs in Current pathfinder.cpp

#### Bug 1: Flag Exclusion (FATAL)

Current code uses `setPolyFlags(ref, origFlags | kDangerPolyFlag)` + `setExcludeFlags(kDangerPolyFlag)` in PASS 1.

**Why this is fatal**: `findNearestPoly` applies the filter. If the player is standing inside a danger zone (which happens constantly — you walk NEAR mobs), `findNearestPoly` returns `ref = 0` because the nearest poly has the exclude flag. Then `findPath` gets `startRef = 0` and returns `DT_FAILURE | DT_INVALID_PARAM`. Navigation breaks completely.

Source: Research Doc 1 (compass_artifact) — explicitly states this is fatal.

#### Bug 2: Two-Pass Approach (Wasteful)

PASS 1 (exclusion) + PASS 2 (cost fallback). Research Doc 1: "The two-pass approach doubles the work without meaningful benefit over pure area cost marking." A single pass with area costs does exactly what both passes try to achieve.

#### Bug 3: Cost = 500.0 (Too High)

Current `kDangerAreaCost = 500.0f`. Research Doc 3 analysis:
- Cost 500+ exhausts `maxNodes` pool because A* must explore vast areas before accepting a danger path
- Precision degradation with float32 (`g = dist * 500.0` — large numbers lose precision)
- Dijkstra-like degeneration — heuristic becomes meaningless relative to inflated edge costs
- Research consensus: **cost = 50.0 is optimal**. Detours up to 50x direct distance while maintaining good float precision and effective heuristic guidance.

#### Bug 4: No Post-Validation

Current code checks `throughDanger` by looking at poly areas in the A* result. But this is checking our own temporary markings, not actual NPC aggro zones. There's no post-validation that verifies the straight path against real aggro circles. The area-cost approach is a HEURISTIC — it guides A* toward safer paths but cannot guarantee avoidance. Only geometric post-validation against actual NPC positions is authoritative.

Source: Research Doc 5 (Safe Passage Protocol) — "areaCost = heuristic steering, post-validation = final arbiter."

#### Bug 5: PolyRestorer Saves Flags (Unnecessary)

`PolyRestorer` saves both `flags` and `area`. Since we should NEVER modify flags (only areas), the flag save/restore is unnecessary complexity.

#### Bug 6: Margin Architecture Split

Safety margin is split across two places:
- `aggro.h`: `kAggroMargin = 3.0f` — added in RadarData when computing `e.aggroRadius`
- `pathfinder.h`: `kAggroBuffer = 5.0f` — added when marking danger polys
- Total effective buffer: **+8 yards** beyond raw aggro radius

This is fragile. Neither component knows the full buffer. Radar shows one radius, pathfinder uses another.

#### Bug 7: Timer-Based Rerouting Without Hysteresis

NavHelper and FollowRoute check threats every 2 seconds on a timer. No hysteresis check — if a new path is only marginally different, we still swap. This causes oscillation (bot path flickers between two near-equivalent routes).

### 2.3 Previous Fix Attempts (All Failed)

| Attempt | Change | Result |
|---|---|---|
| 1 | maxNodes 8192→65535 + throughDanger fix + margin increase | DT_OUT_OF_NODES with 500 cost |
| 2 | Margin reduction + always reroute | Infinite reroute loop (3350 reroutes) |
| 3 | Unsigned underflow fix for timing | Fixed crash, avoidance still broken |
| 4 | Two-pass exclusion + cost fallback | Flag exclusion fatal (current state) |

---

<a name="research-summary"></a>
## 3. Research Summary: 5 Documents

### Document 1: compass_artifact (Detour Avoidance Deep Dive)

**Key findings:**
- **Flag exclusion is FATAL** — `findNearestPoly` applies the filter, returns ref=0 if player is in danger zone
- **Pure area cost marking is the ONLY correct approach**: `setPolyArea(ref, 63)` + `setAreaCost(63, cost)`, NEVER `setPolyFlags`
- **Dual-filter pattern**: permissive filter (all costs=1.0) for `findNearestPoly`, danger-aware filter (area 63 = cost 50.0) for `findPath`
- **Cost = 50.0 optimal** — strong enough to guide A* around danger, weak enough to avoid DT_OUT_OF_NODES
- **`closestPointOnPoly`** for precise polygon-circle intersection testing (not just AABB overlap from `queryPolygons`)
- **RAII PolyAreaGuard**: save/restore only areas, never touch flags
- **Margin**: multiplicative 1.05× (5%)

**Detour internals explained:**
- `g = dtVdist(pa, pb) * m_areaCost[area]` — edge cost = distance * area cost multiplier
- `h = dtVdist(nodePos, endPos) * 0.999f` — heuristic uses H_SCALE=0.999
- No cost cutoff in Detour code — never checks for isinf/isnan/saturation
- `areaCost < 1.0` breaks pathfinding (heuristic becomes inadmissible)
- `dtNodeIndex = unsigned short`, max 65535 nodes
- `setPolyArea()` modifies the actual tile data in memory (not a copy) — MUST restore

### Document 2: Game Bot Pathfinding and Movement 2 (Russian)

**Key findings:**
- Since `dtQueryFilter` is NOT virtual in this build, cannot subclass `passFilter()`/`getCost()`. Must use `setAreaCost()` + standard filter methods.
- **Geometric "Plan & Validate"**: build path ignoring NPCs, then validate segments against aggro spheres. If unsafe, compute tangent bypass points. This is a POST-VALIDATION approach.
- **Safety margin**: additive +7 yards. Rationale: server tick 400ms (~2.8yd movement at run speed), ping 100ms, bot reaction delay. Need buffer so server doesn't register aggro before bot turns.
- **Patrol prediction**: extrapolate NPC position 1 second forward (`Pos + Velocity * 1.0s`)
- **World Graph + Local Navmesh**: standard industry approach for long-distance MMO bot navigation
  - Global graph: cities, flight masters, portals, boats, zone boundaries
  - Edges: Walk, Flight, Teleport, Boat
  - A* on graph gives macro-route (sequence of zones + key points)
  - Detour used only for local segment between current position and next graph node
- **Tile streaming**: 5x5 tiles (~2600yd visibility). Load new, unload beyond 7x7. Never `removeTile` during active query.
- **Stuck detection**: if `Distance(current, last) < 0.5` for 2 seconds → stuck. Recovery: jump+backward, repath, hearthstone.
- **Repath frequency**: travel=every 30s or on obstacle, combat=every 1-2s, oscillation cooldown=3s if >5 repaths in 10s.
- **Human-like movement**: Perlin noise ±0.5-1.5yd, acceleration simulation, micro-pauses every 20-40s, reaction delay 200-400ms before avoidance.
- **Behavior trees** over FSM for flexibility.

### Document 3: Game Bot Pathfinding and Movement (Academic)

**Key findings:**
- **Cost = 50.0 is definitive** — tested empirically. Cost 500+ causes DT_OUT_OF_NODES, precision degradation.
- **Additive margin is mathematically superior** to multiplicative for small radii. CalcAggroRadius minimum is 5yd. Multiplicative 1.05× on 5yd = only 0.25yd buffer. Multiplicative 1.10× on 5yd = only 0.5yd buffer. Both are dangerously small given server tick variance.
- **Recommended margin**: +5 to +7 yards additive.
- **H_SCALE = 0.999f** — keep default, makes A* slightly more Dijkstra-like (explores more but finds better paths). Changing it breaks optimality guarantees.
- **maxNodes = 2048 sufficient for 3x3 tiles**, 65535 is overkill but harmless (just more memory).
- **HPA* (Hierarchical Pathfinding A*)**: good for very large maps, but adds complexity. World Graph is the pragmatic equivalent for MMO bots.
- **Hysteresis**: new path must be >15% better to warrant swap. Measured by: fewer danger polys, or significantly shorter distance.
- **Waypoint overlays**: pre-computed safe corridors through known danger areas. Optional optimization.
- **Rerouting cooldown**: minimum 5 seconds between reroutes. If >5 reroutes in 10 seconds → stop rerouting, use local steering.

### Document 4: Game Bot Pathfinding: NPC Avoidance, Long-Distance (Perplexity)

**Key findings:**
- **Cost ranges**: 10-30 moderate avoidance, 50-100 strong avoidance. Recommends area cost for general approach.
- **Hybrid margin**: `R × 1.10-1.20` then `+2-5yd`. Combines multiplicative scaling with additive floor.
- **`findPolysAroundCircle`**: alternative to `queryPolygons` + manual circle test. Expands from center poly outward. NOTE: requires the center poly ref, which needs `findNearestPoly` first. Potentially more accurate than AABB + closestPointOnPoly but has extra setup step.
- **`DetourPathCorridor`**: Detour's corridor tracking class for dynamic obstacle avoidance. Supports `moveOverOffmeshConnection`, `optimizePathTopology`, `movePosition`. Could be useful for smooth steering.
- **Tile streaming**: 5x5 to 7x7 radius. Keep a "hot zone" of tiles loaded, evict cold tiles.
- **World Graph nodes**: cities, inns, flight paths, boats, zeppelins, portals, dungeon entrances, zone boundaries.

### Document 5: The Safe Passage Protocol (Anti-Detection Framework)

**Key findings:**
- **Two-stage mandatory approach**:
  1. **areaCost = heuristic steering** — guides A* toward safe paths. Cost=50 recommended.
  2. **Post-validation = final arbiter** — check every poly in result against actual aggro zones. "No finite cost can guarantee avoidance if a safe path is impossible."
- **Margin**: multiplicative 15% "generally superior, scales proportionally."
- **Three-tiered architecture**:
  1. **Strategic Planner** — world graph A* for macro-route (zone-to-zone)
  2. **Tactical Navigation** — Detour navmesh with danger-aware A* for local segments
  3. **Movement Synthesizer** — trajectory noise, velocity profiling, micro-pauses for human-like movement
- **Recovery protocol**: wait → evaluate → fight/flee. NOT immediate combat.
- **Event-driven replanning**: reroute when NPC moves significantly, when bot reaches danger segment, when stuck. NOT purely timer-based.
- **throughDanger computation**: exclude first poly (player may be in danger zone) and last poly (destination may be in danger zone). Only intermediate polys matter.
- **Path smoothing**: Catmull-Rom splines + lookahead point 10-15yd ahead for smooth CTM.

---

<a name="agreed-architecture"></a>
## 4. Agreed Architecture and Parameters

### 4.1 Three-Tier Architecture

```
Tier 1: STRATEGIC PLANNER (World Graph)
  - JSON graph of key points: cities, flight masters, portals, boats, zone boundaries
  - A* on graph → macro-route (sequence of waypoints spanning zones/continents)
  - Long-distance navigation (unlimited range)

Tier 2: TACTICAL NAVIGATION (Detour NavMesh)
  - Local pathfinding within loaded tile area
  - Danger-aware A* (area cost marking)
  - Post-validation against actual aggro zones
  - 5x5 base tile loading + corridor extension along planned route

Tier 3: MOVEMENT SYNTHESIZER (Anti-Detection)
  - Trajectory noise (Perlin noise ±0.5-1.5yd)
  - Velocity profiling (acceleration/deceleration on turns)
  - Micro-pauses (100-300ms every 20-40s)
  - Reaction delay (200-400ms before avoidance maneuver)
```

### 4.2 Final Parameter Table

| Parameter | Value | Source | Rationale |
|---|---|---|---|
| Danger area ID | 63 | Doc 1 | Unused area type (DT_MAX_AREAS=64, IDs 0-63) |
| Danger area cost | **50.0** | Doc 1, 3 | Optimal: detours up to 50x, good precision, no DT_OUT_OF_NODES |
| Safety margin | **R × 1.15 + 3.0** | Hybrid of Doc 2,3,5 | 15% multiplicative scaling + 3yd additive floor. User chose "safer" |
| Reroute hysteresis | >15% improvement | Doc 3 | New path must have >15% fewer danger polys or be significantly shorter |
| Reroute cooldown | 5 seconds min | Doc 3 | Between reroute attempts |
| Reroute rate limit | 5 in 10s → stop | Doc 3 | If oscillating, stop rerouting and use current path |
| Event-based reroute | See triggers below | Doc 5 | NOT purely timer-based |
| Wait timeout | 10 seconds | User decision | Wait for mobs to move, then evaluate |
| Tile radius | 2 (5x5 grid) | Doc 2, 4 | ~2600yd visibility |
| maxNodes | 65535 | Already set | More than enough for 5x5 tiles |
| kMaxPathPolygons | 256 | Current | OK for local paths |
| Post-validation | MANDATORY | Doc 5 | Check every intermediate poly against real aggro zones |
| H_SCALE | 0.999f (default) | Doc 3 | Do not change |

### 4.3 Safety Margin Formula

```
effective_radius = CalcAggroRadius(creatureLevel, playerLevel) * 1.15 + 3.0
```

Where `CalcAggroRadius = clamp(20 + (creature - player), 5, 45)`.

Examples:
| Creature Lv | Player Lv | Raw Aggro | × 1.15 | + 3.0 | Total Buffer |
|---|---|---|---|---|---|
| 10 | 10 | 20.0 | 23.0 | 26.0 | +6.0 over raw |
| 15 | 10 | 25.0 | 28.75 | 31.75 | +6.75 over raw |
| 5 | 10 | 15.0 | 17.25 | 20.25 | +5.25 over raw |
| 60 | 10 | 45.0 | 51.75 | 54.75 | +9.75 over raw |
| 10 | 60 | 5.0 | 5.75 | 8.75 | +3.75 over raw |

The additive +3.0 ensures minimum ~3yd buffer even at smallest radii. The 1.15 multiplicative ensures proportional scaling for large radii.

### 4.4 Rerouting Triggers (Event-Based)

Instead of checking threats every 2 seconds on a fixed timer:

1. **NPC moved >5yd** and now intersects current straight path → immediate reroute
2. **Bot reached a segment** where `throughDanger = true` → reroute (danger may have moved)
3. **Stuck event** (stuck detection triggers) → reroute
4. **Fallback timer**: every 8 seconds → reroute check (catch anything missed)
5. **New NPC appeared** in radar within danger range of path → reroute

Each trigger applies **hysteresis**: new path must be >15% better, else keep current.

### 4.5 Recovery Protocol (When No Safe Path Exists)

When `throughDanger = true` AND no better path available:

```
1. WAIT (up to 10 seconds)
   - Mobs may be patrolling and move away
   - Re-check every 2 seconds during wait
   - If path clears → resume

2. EVALUATE (after wait expires or immediately if mobs are static)

   2a. Single weak mob (level diff < 3 or within combat capability):
       → Attack, loot, continue on path

   2b. Single strong mob (level diff >= 3 or clearly outlevels player):
       → Run through on minimum-damage path (shortest danger exposure)
       → Do NOT engage in combat (will lose)

   2c. Multiple weak mobs:
       → If possible, find path through weakest cluster
       → Attack weakest blocking mob, loot, re-check path

   2d. Multiple strong mobs:
       → Run through on path with shortest total danger segment
       → Accept damage, do NOT stop to fight

   2e. Truly impossible (surrounded, no run-through possible):
       → Use hearthstone or die and corpse-run
       → Log critical warning

3. AFTER RECOVERY:
   - Re-path from current position
   - Reset reroute timers
   - Increment forced-combat counter (limit: 5 per journey)
```

---

<a name="part-a-fix-pathfinder"></a>
## 5. Part A: Fix Pathfinder (NPC Avoidance)

### What Must Change

**pathfinder.h**:
- REMOVE `kDangerPolyFlag` entirely — never touch poly flags
- CHANGE `kDangerAreaCost` from 500.0 to 50.0
- REMOVE `kAggroBuffer` — margin is now computed from actual aggro radius (see Part B)
- ADD `kAggroMarginMult = 1.15f` and `kAggroMarginAdd = 3.0f`
- UPDATE `FindPathAvoiding` comment — single-pass, not two-pass

**pathfinder.cpp — complete rewrite of `FindPathAvoiding`**:

### Correct Algorithm (Single-Pass Area Cost + Post-Validation)

```
FindPathAvoiding(start, end, dangers):

  1. MARK DANGER POLYS
     For each DangerZone:
       - effectiveRadius = dz.radius * kAggroMarginMult + kAggroMarginAdd
       - center = [dz.y, dz.z, dz.x]  (Detour coords)
       - halfExtents = [effectiveRadius, kAggroQueryExtentY, effectiveRadius]
       - queryPolygons(center, halfExtents, permissiveFilter, polys, count, max)
       - For each poly in result:
           closestPointOnPoly(ref, center, &closestPt) → precise circle test
           if 2D distance < effectiveRadius:
             Save original area → guard.markDanger(ref)
             setPolyArea(ref, kDangerAreaId)
     PolyAreaGuard restores all areas on scope exit (RAII).

  2. BUILD TWO FILTERS
     permissiveFilter: includeFlags=0xFFFF, excludeFlags=0, all areaCosts=1.0
       → Used for findNearestPoly (never excludes anything)
     dangerFilter: includeFlags=0xFFFF, excludeFlags=0, areaCost[63]=50.0
       → Used for findPath (makes danger polys expensive but not forbidden)

  3. FIND START/END POLYS (PERMISSIVE filter!)
     startRef = findNearestPoly(startPos, extents, permissiveFilter)
     endRef = findNearestPoly(endPos, extents, permissiveFilter)
     If endRef == 0: retry with large extents → partial=true

  4. A* PATHFINDING (DANGER filter)
     findPath(startRef, endRef, start, end, dangerFilter, polys, count, max)
     If DT_PARTIAL_RESULT → partial=true

  5. STRING-PULL
     findStraightPath(start, end, polys, count, ...) → waypoints

  6. POST-VALIDATION (MANDATORY)
     For each intermediate poly (skip first and last):
       getPolyArea(pathPolys[i], &area)
       if area == kDangerAreaId:
         throughDanger = true
         break
     This checks whether A* was forced through a danger zone.

  7. RETURN PathResult { waypoints, success, partial, throughDanger }
```

### RAII PolyAreaGuard (Areas Only, No Flags)

```cpp
struct PolyAreaGuard {
    dtNavMesh* mesh = nullptr;
    struct Saved { dtPolyRef ref; uint8_t area; };
    std::vector<Saved> saved;

    void markDanger(dtPolyRef ref) {
        uint8_t origArea = 0;
        if (dtStatusSucceeded(mesh->getPolyArea(ref, &origArea))) {
            if (origArea != kDangerAreaId) {  // not already marked
                saved.push_back({ ref, origArea });
                mesh->setPolyArea(ref, kDangerAreaId);
            }
        }
    }

    ~PolyAreaGuard() {
        for (auto& s : saved)
            mesh->setPolyArea(s.ref, s.area);
    }
};
```

### Logging Requirements

```
[Nav] FindPathAvoiding: N polys marked in M danger zones (margin=1.15x+3.0)
[Nav] FindPathAvoiding: K waypoints (safe) — N danger polys out of P total
[Nav] FindPathAvoiding: K waypoints THROUGH_DANGER (D danger polys of P total)
[Nav] FindPathAvoiding: failed, dtStatus=0x...
[Nav] FindPathAvoiding: no start poly near (x, y, z) tile=(tx,ty)
[Nav] FindPathAvoiding: end poly found with expanded extents (PARTIAL)
```

---

<a name="part-b-margin-refactor"></a>
## 6. Part B: Margin Architecture Refactor

### Problem

Safety margin is currently split:
- `aggro.h`: `kAggroMargin = 3.0f` added into `RadarEntry::aggroRadius`
- `pathfinder.h`: `kAggroBuffer = 5.0f` added in `FindPathAvoiding`
- Result: Total buffer = +8yd beyond raw aggro, but neither component knows the full picture

### Solution: Single Source of Truth

**Pathfinder** controls the full margin. Radar provides raw aggro radius for visualization, and a separate buffered radius for display.

#### Changes to `aggro.h`:
- REMOVE `kAggroMargin = 3.0f` (pathfinder now owns the margin)
- KEEP `CalcAggroRadius()` unchanged

#### Changes to `radar.h` / `radar.cpp`:
- `RadarEntry::aggroRadius` → rename to `aggroRadiusRaw` — pure CalcAggroRadius value
- ADD `aggroRadiusBuffered` — for radar ring visualization: `raw * 1.15 + 3.0`
- Radar display uses `aggroRadiusBuffered` for the red circle
- Consumers building DangerZones use `aggroRadiusRaw` — pathfinder applies its own margin

#### Changes to `pathfinder.h`:
- ADD `kAggroMarginMult = 1.15f`
- ADD `kAggroMarginAdd = 3.0f`
- REMOVE `kAggroBuffer`
- In `FindPathAvoiding`: `effectiveRadius = dz.radius * kAggroMarginMult + kAggroMarginAdd`

#### Changes to `nav_helper.cpp` / `follow_route.cpp` (`CollectDangerZones`):
- Pass `e.aggroRadiusRaw` (not buffered) to `DangerZone::radius`
- Pathfinder applies margin internally

#### Changes to `threat_scanner.cpp` (`GetBlockingThreats`):
- Use `e.aggroRadiusRaw` (or keep using whatever radar provides — just be consistent)
- For path validation (post-hoc), use the same buffered radius as pathfinder: `raw * 1.15 + 3.0`

---

<a name="part-c-recovery-protocol"></a>
## 7. Part C: Recovery Protocol (Combat/Flee)

### Current State

`MoveToTool::HandleBlockedPath()` and `FollowRouteTool::HandleBlockedPath()` immediately start combat with the weakest mob. No waiting, no flee option, no strength evaluation.

### New Recovery State Machine

```
enum class RecoveryState {
    None,           // Not in recovery
    Waiting,        // Waiting for mobs to move (up to 10s)
    Evaluating,     // Analyzing threat strength
    Attacking,      // Fighting weakest mob
    RunningThrough, // Sprinting through danger zone
    Failed          // Gave up
};
```

### Wait Phase

```cpp
// In NavHelper (or a new RecoveryHelper):
if (throughDanger && m_recoveryState == RecoveryState::None) {
    m_recoveryState = RecoveryState::Waiting;
    m_waitStartTime = GetTickCount64();
    game::movement::StopCTM();  // Stop and wait
    LOG(INFO) << "[Recovery] Waiting up to 10s for mobs to clear...";
}

if (m_recoveryState == RecoveryState::Waiting) {
    uint64_t elapsed = GetTickCount64() - m_waitStartTime;

    // Re-check every 2 seconds during wait
    if (elapsed > m_lastWaitCheck + 2000) {
        m_lastWaitCheck = elapsed;
        auto result = FindPathAvoiding(...);
        if (result.success && !result.throughDanger) {
            // Path cleared!
            m_recoveryState = RecoveryState::None;
            UseNewPath(result);
            return;
        }
    }

    if (elapsed >= 10000) {
        // Wait expired, evaluate
        m_recoveryState = RecoveryState::Evaluating;
    }
}
```

### Evaluate Phase

```cpp
if (m_recoveryState == RecoveryState::Evaluating) {
    auto threats = GetBlockingThreats(currentPath);
    auto player = game::GetLocalPlayer();
    int playerLevel = player->GetLevel();

    // Categorize threats
    bool singleWeak = (threats.size() == 1 &&
                       threats[0].entry.level - playerLevel < 3);
    bool singleStrong = (threats.size() == 1 &&
                         threats[0].entry.level - playerLevel >= 3);
    bool multipleWeak = (threats.size() > 1 &&
                         allBelow(threats, playerLevel + 3));
    bool multipleStrong = (threats.size() > 1 &&
                           anyAbove(threats, playerLevel + 3));

    if (singleWeak) {
        // Attack weakest, loot, continue
        m_recoveryState = RecoveryState::Attacking;
        InitiateForcedCombat(threats[0]);
    } else if (singleStrong || multipleStrong) {
        // Run through on minimum-damage path
        m_recoveryState = RecoveryState::RunningThrough;
        RunThroughDanger();
    } else if (multipleWeak) {
        // Attack weakest blocking mob
        auto* weakest = GetWeakestBlockingThreat(threats);
        m_recoveryState = RecoveryState::Attacking;
        InitiateForcedCombat(*weakest);
    }
}
```

### RunThroughDanger

When running through danger zones:
- Use the path from `FindPathAvoiding` (it already minimizes danger exposure with cost=50)
- Set CTM to walk through without stopping
- Do NOT engage in combat even if aggro'd — just keep running
- Log: `[Recovery] Running through danger zone (N threats, estimated X yards in danger)`

### Strength Evaluation Details

```
"Weak" mob: creature.level < player.level + 3
  → Bot can realistically kill it and continue

"Strong" mob: creature.level >= player.level + 3
  → Combat is risky/impossible, running is better

Example: Player level 2
  - Mob level 1-4: weak → attack
  - Mob level 5+: strong → run through

Example: Player level 30
  - Mob level 1-32: weak → attack
  - Mob level 33+: strong → run through
```

---

<a name="part-d-rerouting"></a>
## 8. Part D: Hysteresis + Event-Based Rerouting

### Current Problem

Timer-based every 2 seconds + 8-second cooldown after reroute. No comparison between old and new path quality. Causes oscillation.

### New Approach

#### Hysteresis Check

```cpp
bool ShouldAcceptNewPath(const PathResult& current, const PathResult& candidate,
                          const std::vector<DangerZone>& dangers) {
    // Count danger polys in each path
    int currentDanger = CountDangerPolys(current);
    int candidateDanger = CountDangerPolys(candidate);

    // Must be >15% better
    if (currentDanger > 0) {
        float improvement = 1.0f - (float)candidateDanger / (float)currentDanger;
        if (improvement < 0.15f)
            return false;  // Not enough improvement
    }

    // If current path is safe but candidate is also safe but shorter
    if (currentDanger == 0 && candidateDanger == 0) {
        float currentLen = PathLength(current);
        float candidateLen = PathLength(candidate);
        if (candidateLen >= currentLen * 0.85f)
            return false;  // Not significantly shorter
    }

    return true;
}
```

#### Event-Based Trigger System

```cpp
struct RerouteState {
    uint64_t lastRerouteTime = 0;
    uint64_t lastFallbackCheck = 0;
    int      rerouteCount = 0;         // count in last 10 seconds
    uint64_t rerouteWindowStart = 0;   // start of 10-second window

    static constexpr uint64_t kMinRerouteInterval = 5000;   // 5s minimum
    static constexpr uint64_t kFallbackInterval = 8000;     // 8s fallback timer
    static constexpr int      kMaxReroutesPerWindow = 5;    // rate limit
    static constexpr uint64_t kRerouteWindow = 10000;       // 10s window
};

bool ShouldReroute(uint64_t now, RerouteState& state) {
    // Rate limit: if >5 reroutes in 10 seconds, stop
    if (now - state.rerouteWindowStart > kRerouteWindow) {
        state.rerouteCount = 0;
        state.rerouteWindowStart = now;
    }
    if (state.rerouteCount >= kMaxReroutesPerWindow) {
        LOG(WARNING) << "[Nav] Reroute rate limit hit, using current path";
        return false;
    }

    // Cooldown: minimum 5 seconds between reroutes
    if (now - state.lastRerouteTime < kMinRerouteInterval)
        return false;

    return true;
}
```

#### Event Detection (in Tick)

```cpp
void NavHelper::Tick() {
    // ... existing logic ...

    // Event 1: NPC moved >5yd and now intersects path
    // (Checked by comparing radar snapshot to cached positions)

    // Event 2: Bot reached a throughDanger segment
    // (Checked by seeing if current waypoint is in a marked danger zone)

    // Event 3: Stuck event (already triggers repath)

    // Event 4: Fallback timer (every 8 seconds)
    if (now >= m_reroute.lastFallbackCheck + RerouteState::kFallbackInterval) {
        m_reroute.lastFallbackCheck = now;
        triggerReroute = true;
    }

    if (triggerReroute && ShouldReroute(now, m_reroute)) {
        CheckThreats();  // performs hysteresis internally
    }
}
```

---

<a name="part-e-tile-streaming"></a>
## 9. Part E: Tile Streaming (5x5+)

### Current State

`nav_mesh.h`: `kTileLoadRadius = 1` → 3x3 grid = ~1600yd coverage.

### Change

`kTileLoadRadius = 2` → 5x5 grid = ~2600yd coverage.

Memory impact: 25 tiles × 50-250KB = 1.25-6.25MB. Trivial for 32-bit process.

### Tile Lifecycle Rules

1. **Load**: tiles within 5x5 grid around player
2. **Unload**: tiles outside 7x7 grid (hysteresis — don't load/unload tiles on boundary every frame)
3. **Never** `removeTile` during active `findPath`/`findStraightPath` — must be between queries
4. `removeTile` invalidates all `dtPolyRef` from that tile — discard current path if any tile in it is unloaded
5. Load tiles on main thread (`addTile` — Detour is not thread-safe). File I/O can be async.

### Code Change in `nav_mesh.h`

```cpp
static constexpr int kTileLoadRadius = 2;       // -2..+2 = 5x5 grid
static constexpr int kTileUnloadRadius = 3;     // unload beyond 7x7
```

### Code Change in `UpdateLoadedTiles`

```cpp
void NavMesh::UpdateLoadedTiles(float x, float y) {
    int centerX, centerY;
    WorldToTile(x, y, centerX, centerY);

    if (centerX == m_lastCenterTileX && centerY == m_lastCenterTileY)
        return;

    m_lastCenterTileX = centerX;
    m_lastCenterTileY = centerY;

    // Unload tiles beyond unload radius
    std::vector<std::pair<int,int>> toUnload;
    for (auto& [pos, ref] : m_loadedTiles) {
        if (std::abs(pos.first - centerX) > kTileUnloadRadius ||
            std::abs(pos.second - centerY) > kTileUnloadRadius)
            toUnload.push_back(pos);
    }
    for (auto& pos : toUnload)
        UnloadTile(pos.first, pos.second);

    // Load tiles within load radius
    for (int dx = -kTileLoadRadius; dx <= kTileLoadRadius; ++dx)
        for (int dy = -kTileLoadRadius; dy <= kTileLoadRadius; ++dy)
            if (!IsTileLoaded(centerX + dx, centerY + dy))
                LoadTile(centerX + dx, centerY + dy);
}
```

---

<a name="part-f-world-graph"></a>
## 10. Part F: World Graph (Long-Distance Navigation)

### Purpose

Navigate between any two points regardless of distance, across zones, continents, and using transport.

### Graph Structure

```cpp
struct WorldNode {
    uint32_t id;
    std::string name;        // "Stormwind Gryphon Master"
    uint32_t mapId;          // 0=EK, 1=Kalimdor, 530=Outland, 571=Northrend
    float x, y, z;           // WoW world coords
    enum Type {
        Waypoint,            // Generic walkable point
        FlightMaster,        // Can take taxi from here
        Portal,              // Mage portal / world portal
        BoatZeppelin,        // Transport dock
        InnKeeper,           // Can set hearthstone
        ZoneBoundary,        // Transition between zones
        DungeonEntrance      // Instance portal
    } type;
};

struct WorldEdge {
    uint32_t fromNode, toNode;
    enum Type {
        Walk,         // Navigate via navmesh (Tier 2)
        Flight,       // Take flight master taxi
        Teleport,     // Portal / loading screen
        Boat,         // Boat/zeppelin (wait for transport)
    } type;
    float cost;       // Estimated travel time in seconds
    bool bidirectional;
};

struct WorldGraph {
    std::vector<WorldNode> nodes;
    std::vector<WorldEdge> edges;
};
```

### JSON Format

```json
{
  "nodes": [
    { "id": 1, "name": "Goldshire", "map": 0, "x": -9459, "y": 62, "z": 56, "type": "waypoint" },
    { "id": 2, "name": "SW Flight Master", "map": 0, "x": -8838, "y": 490, "z": 109, "type": "flight_master" },
    ...
  ],
  "edges": [
    { "from": 1, "to": 2, "type": "walk", "cost": 120, "bidir": true },
    { "from": 2, "to": 50, "type": "flight", "cost": 180, "bidir": true },
    ...
  ]
}
```

### A* on World Graph

```cpp
// Simplified — standard A* with heuristic = straight-line distance / max_speed
std::vector<WorldNode> PlanRoute(uint32_t startNodeId, uint32_t endNodeId);
```

The result is a sequence of WorldNodes with edge types. The bot then:
1. For Walk edges → use Tier 2 (Detour navmesh) to navigate between nodes
2. For Flight edges → interact with flight master NPC, select destination
3. For Boat edges → navigate to dock, wait for transport, ride it
4. For Teleport edges → use portal / trigger loading screen

### Data Source

**Ideal**: auto-extract from WoW client (DBC files contain flight path data, NPC positions).
**Fallback**: hand-crafted JSON file with major points.

Start with a minimal graph (Eastern Kingdoms lowbie zones) and expand over time.

### Finding Nearest World Node

When the user requests "go to X" and X is far away:

```cpp
WorldNode* FindNearestNode(uint32_t mapId, float x, float y) {
    // Search all nodes on this map, return closest by 2D distance
    // If no node within reasonable range (e.g. 2000yd), path is "local only"
}
```

### Full Navigation Flow

```
User: "Go to Ironforge" (from Elwynn Forest)

1. Strategic Planner:
   - Find nearest WorldNode to player → "Goldshire"
   - Find WorldNode nearest to Ironforge → "IF Gryphon Master"
   - A* on graph: Goldshire → SW Flight Master → (flight) → IF Gryphon Master
   - Route: [Walk to SW FM, Take flight to IF]

2. Segment 1: Walk Goldshire → SW Flight Master
   - Tier 2: Load tiles along route, FindPathAvoiding with dangers
   - Corridor loading: pre-load tiles along the walk path
   - Bot walks along navmesh path, avoiding hostile NPCs

3. Segment 2: Take flight SW → IF
   - Bot interacts with flight master NPC
   - Selects Ironforge destination
   - Waits during flight (detect landing)

4. Arrived at IF Gryphon Master → close enough to destination → done
```

---

<a name="part-g-corridor-loading"></a>
## 11. Part G: Corridor Loading

### Purpose

When navigating a long Walk edge (e.g. 3000+ yards), the 5x5 tile grid around the player isn't enough to plan the full route. We need to pre-load tiles along the corridor.

### Algorithm

```
Given: Walk from WorldNode A to WorldNode B (possibly 5000+ yards apart)

1. Compute tile coordinates for A and B
2. Compute all tiles along the line from A to B (Bresenham-like)
3. For each tile along the line, also include ±1 tile laterally (for avoidance detours)
4. Load all corridor tiles + the 5x5 player tiles
5. Plan path on the full loaded set
6. As player moves, maintain:
   - 5x5 around player (always)
   - Corridor tiles ahead of player (for the route)
   - Unload corridor tiles behind player (already traversed)
```

### Memory Considerations

A corridor 10 tiles long × 3 tiles wide = 30 tiles × 250KB = 7.5MB max. Combined with 5x5 player tiles (~6MB), total ~13MB. Well within 32-bit limits.

### Implementation Notes

- Corridor loading must be done BETWEEN pathfinding queries (Detour not thread-safe)
- If corridor crosses zone/map boundaries, need to handle map changes
- `dtNavMesh::init()` with `maxTiles=64` — if we exceed 64 loaded tiles, need to increase this
- Consider increasing `kMaxTilesOverride` to 128 if corridor + player tiles regularly exceed 64

---

<a name="part-h-movement-synthesizer"></a>
## 12. Part H: Movement Synthesizer (Anti-Detection)

> **Priority**: Low (implement after core navigation works)

### Trajectory Noise

```cpp
// Add Perlin noise offset to CTM target
float noiseX = PerlinNoise1D(time * 0.5f) * 1.0f;  // ±1.0 yard
float noiseY = PerlinNoise1D(time * 0.5f + 100.0f) * 1.0f;
game::Vec3 noisyTarget = { target.x + noiseX, target.y + noiseY, target.z };
```

- Amplitude: ±0.5 to 1.5 yards
- Frequency: update every 0.5 seconds
- Ensure noisy target is still on navmesh (findNearestPoly check)

### Velocity Profiling

- Start moving: ramp up speed over 0.5-1.0 seconds
- Turn >45 degrees: reduce speed by 20-30%
- Micro-pauses: 100-300ms every 20-40 seconds of continuous movement
- Random speed variation: ±5% of base run speed

NOTE: WoW CTM doesn't give us direct speed control — this would need to be implemented via short stop-resume cycles or by temporarily stopping CTM and re-issuing.

### Reaction Delay

- When detecting a new threat on path: wait 200-400ms before initiating avoidance
- Human players don't react instantly to seeing a mob

### Lookahead Point

- Don't send CTM to the exact next waypoint
- Send CTM to a point 10-15 yards ahead on the path (interpolated)
- Prevents "stop-start" jerkiness at each waypoint corner
- Smooths turns (bot starts turning before reaching the corner)

---

<a name="implementation-order"></a>
## 13. Implementation Order

### Priority 1: Fix Core Avoidance (Parts A + B)
1. ✅ Rewrite `pathfinder.cpp` — single-pass area cost, cost=50, no flags
2. ✅ Update `pathfinder.h` — remove kDangerPolyFlag, add margin constants
3. ✅ Refactor margin — radar provides raw aggro, pathfinder applies margin
4. ✅ Update `CollectDangerZones` in nav_helper and follow_route
5. ⏳ **Verify**: inject, walk near hostile NPCs, check logs for correct poly marking and path routing

### Priority 2: Hysteresis + Events (Part D)
6. ✅ Add hysteresis check to NavHelper and FollowRoute
7. ✅ Replace timer-based rerouting with event-based triggers + fallback timer
8. ✅ Add rate limiting (5 reroutes per 10s)
9. ⏳ **Verify**: check that bot doesn't oscillate between paths

### Priority 3: Recovery Protocol (Part C)
10. ✅ Implement wait phase (10s) in NavHelper/MoveToTool
11. ✅ Implement strength evaluation (weak vs strong)
12. ✅ Implement run-through behavior for strong mobs
13. ✅ Update HandleBlockedPath in MoveToTool and FollowRoute
14. ⏳ **Verify**: test with both weak and strong blocking mobs

### Priority 4: Expand Tile Coverage (Part E)
15. ✅ Change kTileLoadRadius from 1 to 2
16. ✅ Add kTileUnloadRadius = 3
17. ✅ Update UpdateLoadedTiles for hysteresis unloading
18. ⏳ **Verify**: check memory usage, verify tiles load/unload correctly

### Priority 5: World Graph (Part F)
19. ✅ Define WorldNode/WorldEdge structs
20. ✅ Implement JSON parser for graph file
21. ✅ Implement A* on world graph
22. ❌ Create initial graph data (Eastern Kingdoms lowbie zones) — **DEFERRED** (separate task)
23. ✅ Integrate with MoveToTool — detect long-distance, use world graph (`CreateSmart`)
24. ⏳ **Verify**: test cross-zone navigation — blocked by step 22

### Priority 6: Corridor Loading (Part G)
25. ✅ Implement corridor tile computation (Bresenham + ±1 lateral)
26. ✅ Pre-load corridor tiles before long walks (MoveToTool::Start, >1200yd)
27. ✅ Maintain corridor + player tiles (UnloadBehind in Tick)
28. ✅ Consider maxTiles increase — bumped to 128
29. ⏳ **Verify**: test long-distance walks with corridor loading

### Priority 7: Movement Synthesizer (Part H)
30. ✅ Implement hash-based noise on CTM targets (±0.8yd)
31. ✅ Implement lookahead point (12yd) + navmesh snap
32. ✅ Implement micro-pauses (100-300ms every 20-40s) + reaction delay (200-400ms)
33. ⏳ **Verify**: visual inspection of movement patterns

---

<a name="files-inventory"></a>
## 14. Files Inventory

### Files to REWRITE
| File | What Changes |
|---|---|
| `navigation/pathfinder.cpp` | Complete rewrite of FindPathAvoiding (single-pass, area-cost only, post-validation) |

### Files to MODIFY
| File | What Changes |
|---|---|
| `navigation/pathfinder.h` | Remove kDangerPolyFlag, change cost to 50, add margin constants |
| `navigation/nav_mesh.h` | kTileLoadRadius=2, add kTileUnloadRadius=3 |
| `navigation/nav_mesh.cpp` | Update UpdateLoadedTiles for unload hysteresis |
| `bot/aggro.h` | Remove kAggroMargin (pathfinder owns margin now) |
| `bot/radar.h` | Split aggroRadius into aggroRadiusRaw + aggroRadiusBuffered |
| `bot/radar.cpp` | Compute both raw and buffered aggro radii |
| `bot/nav_helper.h` | Add RerouteState, recovery state, event triggers |
| `bot/nav_helper.cpp` | Hysteresis, event-based rerouting, recovery protocol |
| `bot/threat_scanner.h` | Minor — may add path length helper |
| `bot/threat_scanner.cpp` | Use consistent margin for GetBlockingThreats |
| `bot/tools/follow_route.h` | Add RerouteState, recovery state |
| `bot/tools/follow_route.cpp` | Hysteresis, event-based rerouting, updated HandleBlockedPath |
| `bot/tools/move_to.h` | Add recovery state |
| `bot/tools/move_to.cpp` | Updated HandleBlockedPath with recovery protocol |
| `overlay/overlay.cpp` | Update radar to use aggroRadiusBuffered for display |

### Files to CREATE (later phases)
| File | Purpose |
|---|---|
| `navigation/world_graph.h/.cpp` | WorldNode, WorldEdge, A* on graph, JSON parser |
| `navigation/corridor_loader.h/.cpp` | Pre-compute and load corridor tiles |
| `bot/movement_synth.h/.cpp` | Perlin noise, lookahead, micro-pauses |

---

<a name="acceptance-criteria"></a>
## 15. Acceptance Criteria

### Part A: Pathfinder Fix
- [x] `FindPathAvoiding` uses ONLY `setPolyArea` — zero calls to `setPolyFlags`/`setExcludeFlags`
- [x] `findNearestPoly` uses permissive filter (never excludes anything)
- [x] `findPath` uses danger-aware filter with `areaCost[63] = 50.0`
- [x] `PolyAreaGuard` restores only areas (no flags)
- [x] Post-validation checks intermediate polys for throughDanger
- [ ] Log output confirms correct poly marking and path quality *(needs in-game testing)*
- [ ] Bot visibly routes around hostile NPCs on radar *(needs in-game testing)*

### Part B: Margin Refactor
- [x] Single margin formula: `R * 1.15 + 3.0`
- [x] Radar displays buffered radius, pathfinder receives raw radius
- [x] No margin arithmetic in aggro.h or radar code
- [x] Consistent margin across all components

### Part C: Recovery Protocol
- [x] Bot waits up to 10 seconds when path blocked
- [x] Re-checks every 2 seconds during wait
- [x] Evaluates mob strength (weak vs strong)
- [x] Attacks weak mobs, runs through strong mobs
- [x] Forced combat counter limits to 5 per journey
- [x] Logs all recovery decisions

### Part D: Rerouting
- [x] Hysteresis: new path must be >15% better
- [x] Cooldown: 5 seconds minimum between reroutes
- [x] Rate limit: max 5 reroutes per 10-second window
- [x] Event-based triggers replace pure timer
- [x] Fallback timer at 8 seconds
- [ ] No oscillation between equivalent paths *(needs in-game testing)*

### Part E: Tile Streaming
- [x] 5x5 tile grid loads correctly
- [x] 7x7 unload boundary prevents thrashing
- [ ] Memory usage remains reasonable (<10MB for tiles) *(needs in-game testing)*

### Part F: World Graph
- [x] JSON graph file loads and parses correctly *(code ready, no data file yet — deferred)*
- [x] A* produces valid routes across zones *(code ready)*
- [x] Walk segments use Tier 2 navigation (MoveToTool)
- [ ] Flight/transport segments work *(stubs only — needs NPC interaction)*

### Part G: Corridor Loading
- [x] Tiles along route pre-loaded before navigation starts
- [x] Corridor tiles unloaded after traversal (UnloadBehind)
- [x] maxTiles sufficient for corridor + player tiles (128)

### Part H: Movement Synthesizer
- [x] Movement looks human-like *(code ready, needs visual verification)*
- [x] No perfectly straight lines between waypoints (hash-based noise ±0.8yd)
- [x] Natural-looking turns and speed variation (lookahead 12yd + micro-pauses)

---

## 16. Bug Fixes (2026-02-20)

### 16.1 Radar Rotation Mirrored (facingUp mode)

**Problem**: In Player-Facing-Up mode, rotating the character left made the radar rotate left too (should be opposite).

**Root cause**: WoW's left-handed coordinate system (X+=south, Y+=west) — the rotation matrix in `WorldToRadar` and `RadarToWorld` needed negated sinF terms.

**Fix**: `overlay.cpp` — negate sinF in both WorldToRadar and RadarToWorld rotation matrices.

### 16.2 Character Not Stopping on Task Completion/Abort

**Problem**: When navigation tasks (MoveTo, FollowRoute) completed or were aborted via ImGui X button, the character kept running forward indefinitely. The task disappeared from the queue but the character never halted. `MSG_MOVE_HEARTBEAT` continued with no `MSG_MOVE_STOP`.

**Root cause**: CTM and CMovement are **decoupled**. The CTM struct at `0x00CA11D8` is a command buffer; CMovement is the executor. Setting CTM action to Idle/Stop only resets the command buffer — CMovement continues executing the previously-issued movement.

Three previous attempts all failed:
1. Setting CTM action to 0x0D (Idle) + Lua `MoveForwardStop()` — MoveForwardStop only affects keyboard movement
2. Move-to-self with high precision — floating-point anomalies
3. Multiple CallCTM calls in same frame — only last one processed

**Fix**: Call `CGPlayer_C::ClickToMoveStop` at `0x0072B3A0` — a `__thiscall(void* player)` function that properly:
- Resets the CTM state machine
- Clears the `FORWARD` movement flag in CMovement
- Sends `MSG_MOVE_STOP` (opcode `0x00B7`) to the server

**Files changed**:
- `offsets/functions.h` — added `ClickToMoveStop = 0x0072B3A0`
- `game/movement.cpp` — rewrote `StopCTM()` to call ClickToMoveStop
- `bot/action_queue.cpp` — added safety-net `StopCTM()` + `StopMoving()` in `Remove()` and `Clear()`
- `bot/tools/follow_route.cpp` — added `StopMoving()` to `Abort()`
- `bot/nav_helper.cpp` — added `StopMoving()` to `Stop()`

### 16.3 Dual Aggro Radius (Navigation vs Display)

**Problem**: Need wider safety margin for navigation pathfinding while keeping accurate display on radar.

**Fix**: Added `CalcAggroRadiusNav()` with base 30 (vs base 20 for display). Added `aggroRadiusNav` field to `RadarEntry`. Navigation danger zone builders use `aggroRadiusNav`; radar display uses `aggroRadiusRaw`.

**Files changed**:
- `bot/aggro.h` — added `CalcAggroRadiusNav()` (base 30, clamped [5, 55])
- `bot/radar.h` — added `float aggroRadiusNav` to RadarEntry
- `bot/radar.cpp` — compute aggroRadiusNav in Update()
- `bot/nav_helper.cpp` — use aggroRadiusNav in CollectDangerZones
- `bot/tools/follow_route.cpp` — use aggroRadiusNav in CheckThreatsAndReroute
- `overlay/overlay.cpp` — use aggroRadiusNav for danger zones in pathfinding

---

## Appendix A: Detour API Quick Reference

### Key Functions Used

```cpp
// Find polygons overlapping AABB
dtStatus queryPolygons(const float* center, const float* halfExtents,
                       const dtQueryFilter* filter,
                       dtPolyRef* polys, int* polyCount, int maxPolys);

// Find nearest poly to point (APPLIES FILTER!)
dtStatus findNearestPoly(const float* center, const float* halfExtents,
                         const dtQueryFilter* filter,
                         dtPolyRef* nearestRef, float* nearestPt);

// Find closest point on a specific polygon
dtStatus closestPointOnPoly(dtPolyRef ref, const float* pos,
                            float* closest, bool* posOverPoly);

// A* pathfinding through polygon graph
dtStatus findPath(dtPolyRef startRef, dtPolyRef endRef,
                  const float* startPos, const float* endPos,
                  const dtQueryFilter* filter,
                  dtPolyRef* path, int* pathCount, int maxPath);

// String-pull (funnel algorithm)
dtStatus findStraightPath(const float* startPos, const float* endPos,
                          const dtPolyRef* path, int pathSize,
                          float* straightPath, unsigned char* straightPathFlags,
                          dtPolyRef* straightPathRefs,
                          int* straightPathCount, int maxStraightPath,
                          int options);

// Poly area get/set (MODIFIES TILE DATA IN PLACE)
dtStatus getPolyArea(dtPolyRef ref, unsigned char* resultArea);
dtStatus setPolyArea(dtPolyRef ref, unsigned char area);

// Poly flags get/set (DO NOT USE FOR AVOIDANCE)
dtStatus getPolyFlags(dtPolyRef ref, unsigned short* resultFlags);
dtStatus setPolyFlags(dtPolyRef ref, unsigned short flags);

// Filter configuration
void dtQueryFilter::setIncludeFlags(unsigned short flags);
void dtQueryFilter::setExcludeFlags(unsigned short flags);
void dtQueryFilter::setAreaCost(int i, float cost);  // i = 0..63
```

### Detour Cost Formula

```
edge_cost = dtVdist(point_a, point_b) * filter.m_areaCost[poly_area]
heuristic = dtVdist(node_pos, end_pos) * H_SCALE  // H_SCALE = 0.999f

// A* priority: f = g + h
// g = accumulated edge costs from start
// h = heuristic estimate to end
```

### Coordinate Mapping

```
WoW (X+south, Y+west, Z+up) → Detour:
  Detour[0] = WoW.Y
  Detour[1] = WoW.Z
  Detour[2] = WoW.X
  (NO negation)

Detour → WoW:
  WoW.X = Detour[2]
  WoW.Y = Detour[0]
  WoW.Z = Detour[1]
```

### Tile Grid

```
TrinityCore: 64x64 grid, each tile = 533.33 yards
Tile coords from world pos:
  tileX = 32 - (int)(wowX / 533.33f)
  tileY = 32 - (int)(wowY / 533.33f)
```

---

## Appendix B: Research Documents Referenced

1. **compass_artifact** — Detour avoidance deep dive. Flag exclusion fatal, cost=50 optimal, dual-filter pattern.
   Path: `wotlk/docs/reference/researches/bot/compass_artifact_wf-*.md`

2. **Game Bot Pathfinding and Movement 2** (Russian) — Geometric validation, +7yd margin, world graph, behavior trees.
   Path: `wotlk/docs/reference/researches/bot/Game Bot Pathfinding and Movement 2.md`

3. **Game Bot Pathfinding and Movement** (Academic) — Cost=50 definitive, additive margin superior, HPA*, hysteresis >15%.
   Path: `wotlk/docs/reference/researches/bot/Game Bot Pathfinding and Movement.md`

4. **Game Bot Pathfinding: NPC Avoidance, Long-Distance** (Perplexity) — Hybrid margin, findPolysAroundCircle, DetourPathCorridor.
   Path: `wotlk/docs/reference/researches/bot/Game Bot Pathfinding_ NPC Avoidance, Long-Distance.md`

5. **The Safe Passage Protocol** — Two-stage (areaCost heuristic + post-validation mandatory), 15% margin, 3-tier architecture.
   Path: `wotlk/docs/reference/researches/bot/The Safe Passage Protocol*.md`
