# Road Graph Integration (Regional Navigation Layer)

**Date**: 2026-03-01
**Task**: `wotlk/docs/tasks/nav-road-graph-integration.md`
**Status**: Working (verified in-game 2026-03-02)

---

## Summary

Integrated the Road Graph as a **Tier 2 regional navigation layer** between the strategic World Graph (Tier 3) and tactical Detour navmesh (Tier 1). The bot now **prefers walking on roads** when available for medium-distance movement (>50 yards).

## Navigation Tier Architecture

```
Tier 3 — Strategic (World Graph)     >2000 yd   cross-zone, flights, boats
Tier 2 — Regional (Road Graph)       >50 yd     follow roads between POIs    [NEW]
Tier 1 — Tactical (Detour NavMesh)   <50 yd     local movement, mob avoidance
```

## Files Created

| File | Purpose |
|---|---|
| `src/navigation/road_graph.h` | RoadNode, RoadEdge, SpatialGrid, RoadGraph, RoadPlan, Bridge structs |
| `src/navigation/road_graph.cpp` | JSON loading, A* pathfinding, spatial grid, PlanRoadPath, ComputeBridges |
| `src/bot/tools/road_nav.h` | RoadNavTool class (three-phase road executor) |
| `src/bot/tools/road_nav.cpp` | Approach/RoadFollow/Departure phases, danger avoidance, skip logic |

## Files Modified

| File | Changes |
|---|---|
| `src/bot/tool.h` | Added `ToolType::RoadNav` to enum |
| `src/bot/tools/move_to.cpp` | `CreateSmart()` now checks Tier 2 road routing before falling back to navmesh |
| `src/bot/tools/strategic_nav.cpp` | Walk segments try road routing first; uses source node's mapId |
| `src/dllmain.cpp` | Loads `Azeroth_roads.json` on startup, computes WorldGraph-RoadGraph bridges |
| `wotlk.vcxproj` | Added new .h and .cpp files to project |

## Key Design Decisions

### RoadGraph (Part A)
- **Singleton** with per-map storage (`m_maps[mapId]`)
- **SpatialGrid**: 100-yard cells, O(1) nearest-node lookup (~45-135 distance comparisons)
- **A***: 3D Euclidean heuristic (admissible — cost equals distance). Completes in <1ms for 3600-node graph
- **Node ID 0 reserved** as "not found" sentinel; rejected at load time

### Road Planning (Part C)
- **Cost ratio threshold**: road path must be <3x direct distance
- **Approach distance threshold**: approach must be <50% of direct distance
- Returns `RoadPlan` struct with entry/exit nodes, full path, and cost breakdown

### RoadNavTool (Part D)
- **Three-phase execution**: Approach (navmesh to entry) -> RoadFollow (node-by-node) -> Departure (navmesh to destination)
- **Arrival threshold**: 5.0 yards (wider than NavHelper's 2.5 to account for road node placement)
- **Iterative skip loop** in `StartNextRoadSegment()` — no recursion risk

### Danger Avoidance (Part E)
- **Layer 1**: NavHelper's built-in `FindPathAvoiding()` routes around mobs per segment
- **Layer 2**: Road node pre-check — skips nodes inside aggro radius (buffered + 5yd margin)
- **Layer 3**: >5 consecutive skips -> abandon road, pure navmesh to destination
- **Departure phase**: handles both `Failed` and `Blocked` NavHelper states

### Bridge Computation (Part B)
- Connects WorldGraph POI nodes to nearest road nodes (300yd search radius)
- Computed once after both graphs load; cleared on road graph re-load

### Integration (Part F)
- `CreateSmart()`: Tier 3 (>2000yd) -> Tier 2 (>50yd + road available) -> Tier 1 (navmesh)
- `StrategicNavTool`: Walk segments try `PlanRoadPath` first; uses WorldGraph node's `mapId` (not player's current map)

## Data Dependencies

| File | Location | Status |
|---|---|---|
| `Azeroth_roads.json` | `wotlk/data/` | Ready (3595 nodes, 3653 edges, map 0) |

## Bugfix: Overlay Bypassed Road Routing (2026-03-02)

**Problem**: Road graph loaded successfully (3595 nodes, 80 bridges), but the bot completely ignored roads during gameplay. Zero `[RoadNav]` log entries during navigation.

**Root Cause**: The overlay's navigation buttons ("Find Path & Go" / "Interrupt & Go") bypassed `MoveToTool::CreateSmart()` entirely. They manually called `Pathfinder::FindPath()` (pure Detour navmesh) and created `FollowRouteTool` directly with those waypoints. The road routing logic in `CreateSmart()` was never reached.

**Fix**: Changed both overlay navigation buttons to call `MoveToTool::CreateSmart(destination)` instead of the manual navmesh-only pipeline. `CreateSmart` handles three-tier routing: Strategic (>2000yd) → Road (>50yd) → Navmesh (fallback). Avoidance is handled internally by each tool via `NavHelper::SetAvoidanceEnabled(true)`.

**File changed**: `src/overlay/overlay.cpp` — replaced manual `Pathfinder + FollowRouteTool` creation with `MoveToTool::CreateSmart()` calls.

## Code Review Fixes Applied

- Converted recursive `StartNextRoadSegment`/`SkipCurrentRoadNode` to iterative loop (stack overflow prevention)
- Node ID 0 rejected at load time (sentinel collision)
- `LoadFromFile` clears stale map data and bridges on re-load
- `ComputeBridges` guarded by `IsLoaded(0)` check
- Strategic nav uses source node's `mapId` instead of player's current map
- Departure phase handles `NavHelper::Status::Blocked`
