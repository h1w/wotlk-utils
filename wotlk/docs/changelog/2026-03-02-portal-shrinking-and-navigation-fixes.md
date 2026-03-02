# Portal Shrinking & Navigation Quality Fixes

**Date**: 2026-03-02

---

## Summary

Implemented custom funnel algorithm with portal shrinking to keep paths away from polygon edges (doorframes, stairs, narrow corridors). Fixed critical RoadNav oscillation bug and radar visualization. Removed movement humanization (lookahead, noise, snap) that was causing corner-cutting and route breakage.

## Changes

### 1. Custom Funnel Algorithm (NEW)

**Files created**: `src/navigation/funnel.h`, `src/navigation/funnel.cpp`

Reimplemented Detour's `findStraightPath` (string-pulling) using only public `dtNavMesh` API. The custom funnel contracts each portal (shared edge between adjacent navmesh polygons) inward by a configurable margin before running the funnel algorithm, keeping the resulting path away from polygon edges.

Three components:
- **`GetPortalPoints()`** — reimplements Detour's private `getPortalPoints` using public API (`getTileAndPolyByRef`, struct field access). Handles internal edges, cross-tile boundary clamping (`link.bmin`/`bmax`), and off-mesh connections.
- **`ShrinkPortal()`** — contracts portal endpoints toward each other by `margin` yards in XZ plane. Off-mesh connections are not shrunk.
- **`FunnelStraightPath()`** — extracts portals from polygon corridor, shrinks them, runs Simple Stupid Funnel algorithm (dtTriArea2D tests). Returns `false` on failure to trigger fallback to stock `findStraightPath`.

**Narrow portal collapse**: portals narrower than `3 * margin` are collapsed to their midpoint instead of being shrunk. This forces the funnel to emit a waypoint at the center of tight passages (doorframes, narrow corridors). Without this, straight-through doorways produce only start+end waypoints (no intermediate point at the doorway), and the character walks a straight line that clips the doorframe.

### 2. Pathfinder Integration

**Files modified**: `src/navigation/pathfinder.h`, `src/navigation/pathfinder.cpp`

- Added `BuildStraightPathShrunk()` — calls `FunnelStraightPath`, converts Detour coords to WoW coords, falls back to stock `BuildStraightPath` on failure.
- Modified `FindPathWithFilter()` and `FindPathAvoiding()` to dispatch to `BuildStraightPathShrunk()` when `m_portalMargin > 0`.
- Added `SetPortalMargin()` / `GetPortalMargin()` public API.
- **Default margin**: `kDefaultPortalMargin = 2.0f` yards. With narrow collapse threshold at `3 * margin = 6.0` yards, most doorframes and narrow corridors get forced center waypoints.

### 3. Removed Movement Humanization from NavHelper (CRITICAL FIX)

**File modified**: `src/bot/nav_helper.cpp`

**Problem**: Three humanization features in `IssueCTMToCurrentWP()` and `RefreshCTM()` were causing the character to deviate from the planned path, cutting corners and falling off stairs:

1. **`GetLookaheadPoint()` (lookahead = 12 yards)** — projected the CTM target 12 yards ahead along the path instead of targeting the current waypoint. On a staircase with a sharp turn at the top, the lookahead target was a point past the turn, causing the character to walk in a straight line toward it — cutting the corner and falling off the edge. This was the primary cause of corner-cutting.

2. **`AddNoise()`** — added random XZ offset to the CTM target to mimic human-like movement. Combined with lookahead, this shifted the target further from the intended path, causing the character to miss waypoints entirely and get stuck walking in circles.

3. **`SnapToNavmesh()`** — snapped the noisy target back onto the navmesh. While well-intentioned, the snap could move the target to an adjacent polygon, compounding the deviation from the planned path.

**Fix**: Both `IssueCTMToCurrentWP()` and `RefreshCTM()` now target the exact current waypoint directly via `ClickToMove(m_waypoints[m_currentIndex])`. No lookahead, no noise, no snap. The character follows each waypoint precisely.

**Before**:
```cpp
void NavHelper::IssueCTMToCurrentWP()
{
    // ...
    if (m_humanize) {
        game::Vec3 target = MovementSynth::GetLookaheadPoint(
            m_waypoints, m_currentIndex, myPos, kLookaheadDist);  // 12yd ahead
        target = m_synth.AddNoise(target);                         // random offset
        auto [snapped, ok] = MovementSynth::SnapToNavmesh(target); // snap to mesh
        if (ok) target = snapped;
        game::movement::ClickToMove(target);
    } else {
        game::movement::ClickToMove(m_waypoints[m_currentIndex]);
    }
}
```

**After**:
```cpp
void NavHelper::IssueCTMToCurrentWP()
{
    if (m_currentIndex >= m_waypoints.size())
        return;
    game::movement::ClickToMove(m_waypoints[m_currentIndex]);
}
```

Same simplification applied to `RefreshCTM()`.

### 4. Fixed RoadNav Reroute Oscillation (CRITICAL FIX)

**File modified**: `src/bot/nav_helper.cpp`

**Problem**: `StartNavTo()` reset reroute state with `m_reroute = RerouteState{}`, which zeroed all timestamp fields (`lastFallbackCheck = 0`, `lastRerouteTime = 0`, `windowStart = 0`). Since `GetTickCount64()` returns system uptime (millions of milliseconds):
- Fallback check: `now >= 0 + 8000` was always `true` — `CheckThreats` fired on the very first `Tick` after every `StartNavTo`.
- Cooldown: `now - 0 < 5000` was always `false` — no cooldown protection.

Result: every time a new nav path started, threats were immediately re-evaluated, causing instant rerouting. The character would start a path, immediately get a new path, change direction, get another new path — walking in circles within a few yards.

**Fix**: Initialize time fields to current tick after zeroing:
```cpp
m_reroute = RerouteState{};
uint64_t now = GetTickCount64();
m_reroute.lastFallbackCheck = now;
m_reroute.lastRerouteTime   = now;
m_reroute.windowStart       = now;
```

Now the 8-second fallback timer and 5-second cooldown start from the moment navigation begins, preventing immediate rerouting.

### 5. RoadNav Radar Visualization

**Files modified**: `src/overlay/overlay.cpp`, `src/bot/tools/road_nav.h`

**Problem**: `RenderRadarWidget()` only handled `ToolType::FollowRoute` and `ToolType::MoveTo`. `ToolType::RoadNav` had no case — road navigation routes were invisible on the radar widget.

**Fix**:
- Added `GetDestination()` and `GetDetourWaypoints()` accessors to `RoadNavTool`.
- Added `ToolType::RoadNav` case in `RenderRadarWidget()` that displays the current NavHelper segment path, current waypoint marker, destination marker, and detour waypoints.
- Added `#include "../bot/tools/road_nav.h"` to overlay.cpp.

### 6. Reduced Arrival Threshold

**File modified**: `src/bot/nav_helper.h`

Changed `kArrivalThreshold` from `2.5f` to `1.2f` yards. The old 2.5-yard threshold counted waypoints as "arrived" while the character was still several yards away, contributing to corner cutting at turns. The reduced threshold ensures the character gets closer to each waypoint before advancing to the next.

## Files Changed

| File | Action |
|------|--------|
| `src/navigation/funnel.h` | Created |
| `src/navigation/funnel.cpp` | Created |
| `src/navigation/pathfinder.h` | Modified (margin API, constants) |
| `src/navigation/pathfinder.cpp` | Modified (funnel dispatch, BuildStraightPathShrunk) |
| `src/bot/nav_helper.h` | Modified (kArrivalThreshold) |
| `src/bot/nav_helper.cpp` | Modified (reroute init, removed humanization) |
| `src/bot/tools/road_nav.h` | Modified (radar accessors) |
| `src/overlay/overlay.cpp` | Modified (RoadNav radar case) |
| `wotlk.vcxproj` | Modified (added funnel.h/cpp) |
