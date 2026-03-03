# Task: Road Graph Integration (Regional Navigation Layer)

> **Status**: DONE (verified in-game 2026-03-02)
> **Created**: 2026-03-01
> **Phase**: 7h (Navigation)
> **Depends on**: nav-hostile-avoidance, nav-navigate-tool
> **Blocks**: nothing

---

## Table of Contents

1. [Goal](#goal)
2. [Context](#context)
3. [Architecture: Three-Tier Navigation](#architecture)
4. [Part A: RoadGraph Class (Loading + A* + Spatial Index)](#part-a)
5. [Part B: Bridge Computation (World Graph ↔ Road Graph)](#part-b)
6. [Part C: Road Path Planning Algorithm](#part-c)
7. [Part D: RoadNavTool (Segment-by-Segment Executor)](#part-d)
8. [Part E: Danger Avoidance on Roads](#part-e)
9. [Part F: Integration into MoveToTool::CreateSmart](#part-f)
10. [Part G: Edge Cases and Fallbacks](#part-g)
11. [Implementation Order](#implementation-order)
12. [Files Inventory](#files-inventory)
13. [Acceptance Criteria](#acceptance-criteria)

---

<a name="goal"></a>
## 1. Goal

Integrate the Road Graph (manually verified road network extracted from ADT textures) as a **regional navigation layer** between the strategic World Graph and tactical Detour navmesh. The bot should **always prefer walking on roads** when possible, falling back to pure navmesh only when no road coverage exists nearby.

Key decisions (agreed in discussion):
- **Priority**: Always prefer roads if available
- **Activation distance**: >50 yards (any non-trivial movement checks road graph)
- **Architecture**: Separate graphs + bridge edges (Road Graph and World Graph remain independent data structures)
- **Following**: Segment-by-segment (navmesh path to each consecutive road node)
- **Z coordinates**: Already correct in road graph data (all 3595 nodes have real Z values, range -12.2 to 495.1)

---

<a name="context"></a>
## 2. Context

### 2.1 What Exists

| Component | File | State |
|---|---|---|
| Road Graph data | `Z:\Games\wow 3.3.5a client\wotlk\data\Azeroth_roads.json` | **Complete** — 3595 nodes, 3653 edges, all Z correct |
| World Graph | `navigation/world_graph.h/.cpp` | Working — A* routing, JSON loading |
| NavHelper | `bot/nav_helper.h/.cpp` | Working — waypoint following, stuck detection, avoidance |
| MoveToTool | `bot/tools/move_to.h/.cpp` | Working — CreateSmart dispatches to strategic/tactical |
| StrategicNavTool | `bot/tools/strategic_nav.h/.cpp` | Working — executes World Graph route segments |
| Pathfinder | `navigation/pathfinder.h/.cpp` | Working — A* with danger avoidance, area cost marking |
| Road Graph (map-editor) | `map-editor/src/data/world_graph_data.h/.cpp` | Working — shared data structures, JSON serialization |

### 2.2 Road Graph Data Format

File: `Azeroth_roads.json` (in wotlk data directory)

```json
{
  "edges": [
    { "bidir": true, "cost": 73.52, "from": 1, "to": 2, "type": "walk" },
    ...
  ],
  "nodes": [
    { "id": 1, "map": 0, "name": "", "type": "waypoint", "x": 3275.1, "y": -3380.8, "z": 142.7 },
    ...
  ]
}
```

Statistics:
- **3595 nodes**, all type "waypoint", all with real XYZ coordinates
- **3653 edges**, all type "walk", all bidirectional, cost = Euclidean distance (yards)
- **Coverage**: Eastern Kingdoms roads (187/687 tiles have roads)
- **Z range**: -12.2 to 495.1 (real terrain heights)
- **Average edge cost**: ~30 yards

### 2.3 Current Navigation Flow (Before This Task)

```
MoveToTool::CreateSmart(A, B):
  if distance > 2000 → StrategicNavTool (World Graph A*)
  else → MoveToTool (pure navmesh via NavHelper)
```

---

<a name="architecture"></a>
## 3. Architecture: Three-Tier Navigation

### New Navigation Flow

```
MoveToSmart(A, B):
  dist = Distance(A, B)

  if dist > 2000:
      // Tier 3: Strategic (World Graph)
      route = WorldGraph::PlanRoute(nearestWorldNode(A), nearestWorldNode(B))
      for each Walk segment in route:
          ExecuteWalkSegment(segStart, segEnd)  // uses Tier 2
      for each Flight/Boat/Teleport segment:
          ExecuteTransportSegment(...)

  else if dist > 50:
      // Tier 2: Regional (Road Graph)
      ExecuteWalkSegment(A, B)

  else:
      // Tier 1: Tactical (pure navmesh)
      NavHelper::StartNavTo(B)
```

### Tier Diagram

```
Tier 3 — Strategic (World Graph)
   Flight masters, portals, boats, zeppelins
   For cross-zone / cross-continent movement
         │
         ▼  Walk segments
Tier 2 — Regional (Road Graph)          ← THIS TASK
   Follow roads between POIs
   For medium distances (50-2000 yd)
         │
         ▼  Per road node
Tier 1 — Tactical (Detour NavMesh)
   Local movement, mob avoidance
   For short distances (<50 yd)
```

### ExecuteWalkSegment — Core Algorithm

```
ExecuteWalkSegment(start, end):
    entryNode = RoadGraph::FindNearest(start, maxRadius=200yd)
    exitNode  = RoadGraph::FindNearest(end,   maxRadius=200yd)

    if (entryNode && exitNode):
        // Compute costs
        approachDist  = Distance3D(start, entryNode.pos)
        roadPathCost  = RoadGraph::A*(entryNode, exitNode)  // sum of edge costs
        departureDist = Distance3D(exitNode.pos, end)
        totalRoadCost = approachDist + roadPathCost + departureDist
        directDist    = Distance3D(start, end)

        if (totalRoadCost < directDist * 3.0):
            // Use road
            Phase 1: NavHelper → entryNode.pos      (approach)
            Phase 2: Road waypoints segment-by-segment
            Phase 3: NavHelper → end                 (departure)
            return

    // Fallback: pure navmesh
    NavHelper::StartNavTo(end)
```

---

<a name="part-a"></a>
## 4. Part A: RoadGraph Class

### New Files: `navigation/road_graph.h` + `road_graph.cpp`

```cpp
namespace nav {

struct RoadNode {
    uint32_t id;
    float x, y, z;       // WoW world coordinates (all have real Z)
    uint32_t mapId;
};

struct RoadEdge {
    uint32_t from, to;
    float cost;           // Euclidean distance in yards
    // All edges are bidirectional walk — no need for type/bidir fields
};

class RoadGraph {
public:
    static RoadGraph& Instance();

    // Loading
    bool LoadFromFile(uint32_t mapId, const char* jsonPath);
    void Clear();
    bool IsLoaded(uint32_t mapId) const;

    // Queries
    const RoadNode* FindNearestNode(uint32_t mapId, float x, float y, float z,
                                     float maxRadius) const;

    // A* pathfinding — returns ordered node IDs, empty if no path
    std::vector<uint32_t> FindPath(uint32_t fromNodeId, uint32_t toNodeId) const;

    // Get node by ID
    const RoadNode* GetNode(uint32_t id) const;

    // Stats
    uint32_t GetNodeCount(uint32_t mapId) const;
    uint32_t GetEdgeCount(uint32_t mapId) const;

private:
    // Per-map storage
    struct MapData {
        std::vector<RoadNode> nodes;
        std::vector<RoadEdge> edges;
        std::unordered_map<uint32_t, size_t> nodeIndex;  // id → index
        std::unordered_map<uint32_t, std::vector<size_t>> adjacency; // nodeId → edge indices
        SpatialGrid grid;
    };

    std::unordered_map<uint32_t, MapData> m_maps;  // mapId → data
};

} // namespace nav
```

### Spatial Index

Simple grid-based spatial hash for fast nearest-node queries:

```cpp
struct SpatialGrid {
    static constexpr float kCellSize = 100.0f;  // 100 yard cells

    struct CellKey {
        int32_t cx, cy;
        bool operator==(const CellKey& o) const { return cx == o.cx && cy == o.cy; }
    };
    struct CellKeyHash {
        size_t operator()(const CellKey& k) const {
            return std::hash<int64_t>()(((int64_t)k.cx << 32) | (uint32_t)k.cy);
        }
    };

    std::unordered_map<CellKey, std::vector<uint32_t>, CellKeyHash> cells;

    void Insert(uint32_t nodeId, float x, float y);

    // Check 3x3 cells around (x,y), return closest node within maxRadius
    uint32_t FindNearest(float x, float y, float z, float maxRadius,
                          const std::vector<RoadNode>& nodes,
                          const std::unordered_map<uint32_t, size_t>& nodeIndex) const;
};
```

With ~3600 nodes and 100yd cells, most cells have 5-15 nodes. FindNearest checks 9 cells = ~45-135 distance comparisons. O(1) effective performance.

### A* Implementation

Standard A* with 3D Euclidean heuristic:

```cpp
std::vector<uint32_t> RoadGraph::FindPath(uint32_t fromId, uint32_t toId) const {
    // Priority queue: (f-cost, nodeId)
    // gScore: actual cost from start
    // cameFrom: parent tracking
    // Heuristic: Distance3D(current, target) — admissible

    // Graph is small (3600 nodes) — A* completes in <1ms
    // No need for optimization (bidirectional, landmarks, etc.)
}
```

### JSON Parsing

Reuse nlohmann/json (already in project). Parse same format as map-editor's `WorldGraphData::LoadFromFile`, but into lighter `RoadNode`/`RoadEdge` structs (no name, faction fields needed for DLL).

---

<a name="part-b"></a>
## 5. Part B: Bridge Computation (World Graph ↔ Road Graph)

### Purpose

Connect World Graph POI nodes (flight masters, portals, inns) to their nearest road nodes. This allows Strategic route Walk segments to seamlessly transition into road following.

### Data Structure

```cpp
struct Bridge {
    uint32_t worldNodeId;   // World Graph node ID
    uint32_t roadNodeId;    // Nearest Road Graph node ID
    float distance;         // 3D distance between them
};

// In RoadGraph or a separate BridgeRegistry:
std::unordered_map<uint32_t, Bridge> m_bridges;  // worldNodeId → bridge
```

### Computation (on load, after both graphs loaded)

```cpp
void ComputeBridges() {
    auto& wg = WorldGraph::Instance();
    auto& rg = RoadGraph::Instance();

    for (const auto& wnode : wg.GetNodes()) {
        // Only bridge walkable node types
        if (wnode.type == NodeType::FlightMaster ||
            wnode.type == NodeType::Portal ||
            wnode.type == NodeType::BoatZeppelin ||
            wnode.type == NodeType::InnKeeper ||
            wnode.type == NodeType::ZoneBoundary ||
            wnode.type == NodeType::Waypoint) {

            const auto* rnode = rg.FindNearestNode(wnode.mapId,
                                                     wnode.x, wnode.y, wnode.z,
                                                     300.0f);  // 300yd search radius
            if (rnode) {
                m_bridges[wnode.id] = { wnode.id, rnode->id,
                    Distance3D(wnode.pos, rnode->pos) };
            }
        }
    }
    LOG(INFO) << "[RoadGraph] Computed " << m_bridges.size() << " bridges";
}
```

### Usage in Strategic Navigation

When StrategicNavTool executes a Walk segment (nodeA → nodeB):

```
bridgeA = m_bridges[nodeA.id]  // road node near Walk segment start
bridgeB = m_bridges[nodeB.id]  // road node near Walk segment end

if (bridgeA && bridgeB):
    1. NavHelper: nodeA.pos → bridgeA.roadNode.pos    (approach to road)
    2. RoadNavTool: bridgeA → bridgeB via road A*      (road following)
    3. NavHelper: bridgeB.roadNode.pos → nodeB.pos     (departure from road)
else:
    // No road coverage — pure navmesh
    NavHelper: nodeA.pos → nodeB.pos
```

---

<a name="part-c"></a>
## 6. Part C: Road Path Planning Algorithm

### Full Planning Function

```cpp
struct RoadPlan {
    bool useRoad;                    // true = road path found
    uint32_t entryNodeId;            // first road node
    uint32_t exitNodeId;             // last road node
    std::vector<uint32_t> roadPath;  // ordered road node IDs (entry → exit)
    float approachDist;              // distance: start → entry
    float roadDist;                  // total road path distance
    float departureDist;             // distance: exit → end
    float totalDist;                 // sum of all three
    float directDist;                // straight-line start → end
};

RoadPlan PlanRoadPath(const game::Vec3& start, const game::Vec3& end,
                       uint32_t mapId) {
    RoadPlan plan{};
    plan.directDist = Distance3D(start, end);

    // 1. Find nearest road nodes
    const auto* entry = RoadGraph::Instance().FindNearestNode(
        mapId, start.x, start.y, start.z, 200.0f);
    const auto* exit = RoadGraph::Instance().FindNearestNode(
        mapId, end.x, end.y, end.z, 200.0f);

    if (!entry || !exit) {
        plan.useRoad = false;
        return plan;  // no road coverage
    }

    if (entry->id == exit->id) {
        plan.useRoad = false;
        return plan;  // same node, no road routing needed
    }

    // 2. A* on road graph
    auto roadPath = RoadGraph::Instance().FindPath(entry->id, exit->id);
    if (roadPath.empty()) {
        plan.useRoad = false;
        return plan;  // disconnected components
    }

    // 3. Compute costs
    plan.entryNodeId = entry->id;
    plan.exitNodeId = exit->id;
    plan.roadPath = std::move(roadPath);
    plan.approachDist = Distance3D(start, {entry->x, entry->y, entry->z});
    plan.departureDist = Distance3D({exit->x, exit->y, exit->z}, end);

    // Sum road edge costs
    plan.roadDist = 0;
    for (size_t i = 0; i + 1 < plan.roadPath.size(); i++) {
        const auto* a = RoadGraph::Instance().GetNode(plan.roadPath[i]);
        const auto* b = RoadGraph::Instance().GetNode(plan.roadPath[i + 1]);
        plan.roadDist += Distance3D({a->x, a->y, a->z}, {b->x, b->y, b->z});
    }

    plan.totalDist = plan.approachDist + plan.roadDist + plan.departureDist;

    // 4. Cost check: road must not be >3x longer than direct
    if (plan.totalDist > plan.directDist * 3.0f) {
        plan.useRoad = false;
        LOG(INFO) << "[RoadNav] Road path too long: " << plan.totalDist
                  << " vs direct " << plan.directDist
                  << " (ratio " << plan.totalDist / plan.directDist << ")";
        return plan;
    }

    plan.useRoad = true;
    LOG(INFO) << "[RoadNav] Road path: " << plan.roadPath.size() << " nodes, "
              << plan.roadDist << " yd road + "
              << plan.approachDist << " yd approach + "
              << plan.departureDist << " yd departure";
    return plan;
}
```

### Cost Ratio Threshold: 3.0

Roads in WoW typically add 20-50% detour vs straight line. Ratio 1.5-2.0 is a normal road. Ratio 3.0 means the road is 3x longer than direct — beyond what a real player would walk. Anything above 3.0 → fallback to navmesh.

---

<a name="part-d"></a>
## 7. Part D: RoadNavTool (Segment-by-Segment Executor)

### New File: `bot/tools/road_nav.h` + `road_nav.cpp`

```cpp
class RoadNavTool : public IActionTool {
public:
    RoadNavTool(const game::Vec3& start, const game::Vec3& end,
                const RoadPlan& plan);

    void Start() override;
    ToolStatus Tick() override;
    void Abort() override;

private:
    enum class Phase {
        Approach,     // navmesh: current pos → entry road node
        RoadFollow,   // segment-by-segment: road node → road node
        Departure,    // navmesh: exit road node → final destination
        Done,
        Failed
    };

    Phase m_phase = Phase::Approach;
    game::Vec3 m_start, m_end;
    RoadPlan m_plan;
    size_t m_roadIndex = 0;           // current index in m_plan.roadPath
    int m_skippedConsecutive = 0;      // danger skip counter
    NavHelper m_nav;

    static constexpr float kRoadArrivalThreshold = 5.0f;  // yards (wider than default 2.5)
    static constexpr int kMaxConsecutiveSkips = 5;         // abandon road after 5 skips
};
```

### Segment-by-Segment Execution

```cpp
ToolStatus RoadNavTool::Tick() {
    auto player = game::GetLocalPlayer();
    auto pos = player->GetPosition();

    switch (m_phase) {
    case Phase::Approach: {
        auto status = m_nav.Tick();
        if (status == NavHelper::Status::Arrived) {
            m_phase = Phase::RoadFollow;
            m_roadIndex = 0;
            StartNextRoadSegment();
        } else if (status == NavHelper::Status::Failed) {
            // Can't reach road entry — fallback to direct navmesh
            LOG(WARNING) << "[RoadNav] Approach failed, falling back to navmesh";
            m_nav.StartNavTo(m_end);
            m_phase = Phase::Departure;
        }
        break;
    }

    case Phase::RoadFollow: {
        auto status = m_nav.Tick();
        if (status == NavHelper::Status::Arrived ||
            DistanceXY(pos, GetCurrentRoadTarget()) < kRoadArrivalThreshold) {
            // Arrived at current road node — advance
            m_skippedConsecutive = 0;
            m_roadIndex++;
            if (m_roadIndex >= m_plan.roadPath.size()) {
                // All road nodes traversed — departure phase
                m_phase = Phase::Departure;
                m_nav.StartNavTo(m_end);
            } else {
                StartNextRoadSegment();
            }
        } else if (status == NavHelper::Status::Failed ||
                   status == NavHelper::Status::Blocked) {
            // Can't reach this road node — skip it
            SkipCurrentRoadNode();
        }
        break;
    }

    case Phase::Departure: {
        auto status = m_nav.Tick();
        if (status == NavHelper::Status::Arrived) {
            m_phase = Phase::Done;
            return ToolStatus::Completed;
        } else if (status == NavHelper::Status::Failed) {
            m_phase = Phase::Failed;
            return ToolStatus::Failed;
        }
        break;
    }

    case Phase::Done:
        return ToolStatus::Completed;
    case Phase::Failed:
        return ToolStatus::Failed;
    }

    return ToolStatus::Running;
}
```

### StartNextRoadSegment

```cpp
void RoadNavTool::StartNextRoadSegment() {
    if (m_roadIndex >= m_plan.roadPath.size()) return;

    uint32_t nodeId = m_plan.roadPath[m_roadIndex];
    const auto* node = RoadGraph::Instance().GetNode(nodeId);

    // Check if this node is near a threat — skip if dangerous
    auto threats = ThreatScanner::GetNearbyThreats(
        {node->x, node->y, node->z}, 45.0f);

    if (IsNearAnyThreat({node->x, node->y, node->z}, threats, 5.0f)) {
        SkipCurrentRoadNode();
        return;
    }

    m_nav.StartNavTo({node->x, node->y, node->z});
    LOG_EVERY_N(INFO, 10) << "[RoadNav] Segment " << m_roadIndex
                           << "/" << m_plan.roadPath.size()
                           << " → node " << nodeId;
}
```

### SkipCurrentRoadNode

```cpp
void RoadNavTool::SkipCurrentRoadNode() {
    m_skippedConsecutive++;
    m_roadIndex++;

    if (m_skippedConsecutive > kMaxConsecutiveSkips) {
        // Too many dangerous/unreachable nodes — abandon road
        LOG(WARNING) << "[RoadNav] " << m_skippedConsecutive
                     << " consecutive skips, abandoning road";
        m_phase = Phase::Departure;
        m_nav.StartNavTo(m_end);
        return;
    }

    if (m_roadIndex >= m_plan.roadPath.size()) {
        m_phase = Phase::Departure;
        m_nav.StartNavTo(m_end);
        return;
    }

    LOG(INFO) << "[RoadNav] Skipping node (danger/unreachable), trying next";
    StartNextRoadSegment();  // recursive — will skip again if next is also bad
}
```

---

<a name="part-e"></a>
## 8. Part E: Danger Avoidance on Roads

### Problem

Bot follows road waypoints, but hostile mob blocks the road. Bot must:
1. Avoid the mob (not walk into aggro radius)
2. Return to the road after passing the mob
3. Abandon road only if road is heavily blocked

### Solution: NavHelper Avoidance + Road Node Skipping

Two layers of protection:

**Layer 1 — NavHelper's built-in avoidance** (already works):
- When NavHelper builds navmesh path from current position to next road node, it uses `FindPathAvoiding()` with danger zones
- Navmesh A* with area cost=50 routes AROUND the mob
- Bot reaches the road node via a detour through safe terrain
- This handles mobs NEAR the road but not ON the road node

```
Road: ─── wp5 ─── wp6 ─── wp7 ───
              ↗ detour  ↘
         mob on road near wp6

NavHelper path: wp5 → [forest detour around mob] → wp6 → wp7
```

**Layer 2 — Road node skipping** (new, in RoadNavTool):
- Before navigating to a road node, check if the node itself is inside an aggro radius
- If yes, skip this node and target the next safe node
- NavHelper will find a navmesh path that avoids the mob automatically

```
Road: ─── wp5 ─── wp6[MOB] ─── wp7 ─── wp8 ───
                    ↑ skip

RoadNavTool: skip wp6, target wp7
NavHelper: builds path from wp5 → wp7 that avoids mob
Bot path: wp5 → [detour through safe terrain] → wp7 → wp8 (back on road)
```

**Layer 3 — Consecutive skip threshold** (abandon road):
- If >5 consecutive road nodes are dangerous/unreachable
- Road is too heavily blocked to be useful
- Switch to pure navmesh for the remainder

```
Road: ─── wp5 ─── wp6[MOB] ─── wp7[MOB] ─── wp8[MOB] ─── wp9[MOB] ─── wp10[MOB] ─── wp11 ───
                    skip1       skip2         skip3         skip4         skip5 → ABANDON

RoadNavTool: 5 skips → Phase::Departure → NavHelper to final destination
```

### Threat Check Function

```cpp
bool IsNearAnyThreat(const game::Vec3& pos,
                      const std::vector<ThreatInfo>& threats,
                      float margin) {
    for (const auto& t : threats) {
        float dist = DistanceXY(pos, t.position);
        float dangerRadius = t.aggroRadiusRaw * 1.15f + 3.0f + margin;
        if (dist < dangerRadius)
            return true;
    }
    return false;
}
```

The `margin` parameter (5 yd) ensures the bot doesn't stop at the exact edge of aggro radius (arrival threshold could place it inside).

---

<a name="part-f"></a>
## 9. Part F: Integration into MoveToTool::CreateSmart

### Current Code (move_to.cpp)

```cpp
static std::unique_ptr<IActionTool> CreateSmart(const game::Vec3& target) {
    auto player = game::GetLocalPlayer();
    float dist = Distance3D(player->GetPosition(), target);

    if (dist > kStrategicThreshold && WorldGraph::Instance().IsLoaded()) {
        // ... strategic routing ...
    }

    return std::make_unique<MoveToTool>(target);
}
```

### New Code

```cpp
static std::unique_ptr<IActionTool> CreateSmart(const game::Vec3& target) {
    auto player = game::GetLocalPlayer();
    auto pos = player->GetPosition();
    float dist = Distance3D(pos, target);
    uint32_t mapId = game::world::GetMapId();

    // Tier 3: Strategic (World Graph) — cross-zone, >2000 yd
    if (dist > kStrategicThreshold && WorldGraph::Instance().IsLoaded()) {
        // ... existing strategic routing ...
        // NOTE: StrategicNavTool Walk segments should also use road graph
        //       (see Part B — bridge integration in StrategicNavTool)
    }

    // Tier 2: Regional (Road Graph) — >50 yd
    if (dist > kRoadThreshold && RoadGraph::Instance().IsLoaded(mapId)) {
        auto plan = PlanRoadPath(pos, target, mapId);
        if (plan.useRoad) {
            return std::make_unique<RoadNavTool>(pos, target, std::move(plan));
        }
    }

    // Tier 1: Tactical (pure navmesh)
    return std::make_unique<MoveToTool>(target);
}

static constexpr float kRoadThreshold = 50.0f;   // yards
static constexpr float kStrategicThreshold = 2000.0f;  // yards (existing)
```

### StrategicNavTool Modification

In `strategic_nav.cpp`, when executing a Walk segment:

```cpp
void StrategicNavTool::StartWalkSegment(const game::Vec3& from, const game::Vec3& to) {
    uint32_t mapId = game::world::GetMapId();

    // Try road routing for this walk segment
    if (RoadGraph::Instance().IsLoaded(mapId)) {
        auto plan = PlanRoadPath(from, to, mapId);
        if (plan.useRoad) {
            m_currentTool = std::make_unique<RoadNavTool>(from, to, std::move(plan));
            return;
        }
    }

    // Fallback: direct navmesh
    m_currentTool = std::make_unique<MoveToTool>(to);
}
```

---

<a name="part-g"></a>
## 10. Part G: Edge Cases and Fallbacks

### 10.1 No Road Graph for Current Map

`RoadGraph::IsLoaded(mapId)` returns false → skip Tier 2 entirely, use Tier 1 (navmesh). Behavior identical to current system.

### 10.2 Nearest Road Node Too Far (>200 yd)

`FindNearestNode` returns nullptr → fallback to navmesh. Bot is in an area without roads (cave, instance, wilderness).

### 10.3 Entry and Exit in Disconnected Components

Road graph A* returns empty path → fallback to navmesh. Eastern Kingdoms graph has 21 gaps (from validation), so this will happen near disconnected road segments.

### 10.4 Approach to Road Node Fails

NavHelper returns Failed for approach phase → fall back to pure navmesh to final destination (skip road entirely). Road node might be on the other side of a cliff/wall.

### 10.5 Road Path Makes Bot Walk Away From Destination

Can happen if nearest road node is behind the bot. Mitigation: already handled by the 3.0x cost ratio check — if approach+road+departure is >3x direct distance, don't use road.

Additional check: if approach distance > 0.5 * direct distance, don't use road (approaching the road costs more than half the journey).

```cpp
if (plan.approachDist > plan.directDist * 0.5f) {
    plan.useRoad = false;
    LOG(INFO) << "[RoadNav] Approach too far (" << plan.approachDist
              << " yd vs " << plan.directDist << " yd direct)";
    return plan;
}
```

### 10.6 Bot Already on Road

Approach distance ≈ 0, approach phase completes instantly. No special handling needed.

### 10.7 Destination on Road

Departure distance ≈ 0, departure phase completes instantly. No special handling needed.

### 10.8 Bot Dies and Resurrects

Position changed → RoadNavTool should detect position jump and re-plan. In practice, death cancels all active tools, so a new CreateSmart call will happen from fresh position.

### 10.9 Combat Interrupts Movement

RoadNavTool supports Abort(). After combat, a new navigation request starts from current position.

### 10.10 Stuck on Road Segment

NavHelper already has stuck detection (3s check, jump+retry, 5 max retries). If stuck → NavHelper returns Failed → RoadNavTool skips this node → tries next. If 5 consecutive fails → abandon road.

---

<a name="implementation-order"></a>
## 11. Implementation Order

### Priority 1: RoadGraph Core (Part A)
1. Create `navigation/road_graph.h` — RoadNode, RoadEdge, SpatialGrid, RoadGraph class
2. Create `navigation/road_graph.cpp` — JSON loading, A*, spatial index, FindNearestNode
3. Add loading in `dllmain.cpp`: `RoadGraph::Instance().LoadFromFile(0, dataDir + "Azeroth_roads.json")`
4. **Verify**: inject DLL, check logs for successful road graph loading (3595 nodes, 3653 edges)

### Priority 2: Road Planning (Part C)
5. Implement `PlanRoadPath()` function (can be in road_graph.cpp or a separate road_planner.cpp)
6. Add cost ratio check (3.0x) and approach distance check (0.5x direct)
7. **Verify**: unit-test A* on road graph — known start/end should produce valid paths

### Priority 3: RoadNavTool (Part D)
8. Create `bot/tools/road_nav.h` — RoadNavTool class
9. Create `bot/tools/road_nav.cpp` — three-phase executor (approach/road/departure)
10. Implement segment-by-segment following with kRoadArrivalThreshold=5.0

### Priority 4: Danger Avoidance (Part E)
11. Add threat check before each road node (IsNearAnyThreat)
12. Implement skip logic with kMaxConsecutiveSkips=5
13. Implement road abandonment fallback

### Priority 5: Integration (Part F)
14. Modify `MoveToTool::CreateSmart()` — add Tier 2 road routing
15. Modify `StrategicNavTool::StartWalkSegment()` — use road routing for Walk edges
16. **Verify**: inject, test navigation from Goldshire to Stormwind — should follow road

### Priority 6: Bridge Computation (Part B)
17. Implement `ComputeBridges()` — connect World Graph nodes to nearest road nodes
18. Call after both graphs loaded in dllmain.cpp
19. **Verify**: log shows bridges created for flight masters, inns, etc.

### Priority 7: Testing & Tuning
20. Test various scenarios: road following, mob avoidance on road, road abandonment
21. Tune thresholds: maxRadius (200yd), cost ratio (3.0), arrival (5.0yd), max skips (5)
22. Test edge cases: no road coverage, disconnected graph, approach fails

---

<a name="files-inventory"></a>
## 12. Files Inventory

### Files to CREATE

| File | Purpose |
|---|---|
| `navigation/road_graph.h` | RoadNode, RoadEdge, SpatialGrid, RoadGraph class |
| `navigation/road_graph.cpp` | JSON loading, A* pathfinding, spatial index |
| `bot/tools/road_nav.h` | RoadNavTool class (three-phase road executor) |
| `bot/tools/road_nav.cpp` | Approach/RoadFollow/Departure phases, skip logic |

### Files to MODIFY

| File | What Changes |
|---|---|
| `dllmain.cpp` | Add RoadGraph loading + ComputeBridges after WorldGraph load |
| `bot/tools/move_to.cpp` | Add Tier 2 road routing in CreateSmart (kRoadThreshold=50) |
| `bot/tools/strategic_nav.cpp` | Use road routing for Walk segments |

### Data Files Required

| File | Location | Status |
|---|---|---|
| `Azeroth_roads.json` | `wotlk/data/` | **Ready** — 3595 nodes, 3653 edges |
| `Kalimdor_roads.json` | `wotlk/data/` | **Not yet created** |
| `Outland_roads.json` | `wotlk/data/` | **Not yet created** |
| `Northrend_roads.json` | `wotlk/data/` | **Not yet created** |

---

<a name="acceptance-criteria"></a>
## 13. Acceptance Criteria

### Part A: RoadGraph Class
- [ ] `Azeroth_roads.json` loads successfully (3595 nodes, 3653 edges in logs)
- [ ] `FindNearestNode` returns correct nearest node for known positions
- [ ] `FindPath` returns valid A* path between connected nodes
- [ ] A* completes in <1ms for any path in the graph

### Part B: Bridges
- [ ] Bridges computed for World Graph nodes near roads
- [ ] Bridge count logged on startup

### Part C: Road Planning
- [ ] `PlanRoadPath` returns valid plans with correct cost calculations
- [ ] Cost ratio >3.0 correctly triggers fallback to navmesh
- [ ] Approach distance >0.5x direct correctly triggers fallback

### Part D: RoadNavTool
- [ ] Three-phase execution works: approach → road follow → departure
- [ ] Segment-by-segment following with kRoadArrivalThreshold=5.0yd
- [ ] Abort properly stops movement

### Part E: Danger Avoidance
- [ ] Mob near road node → node skipped, bot detours via navmesh
- [ ] Mob ON road node → node skipped, next safe node targeted
- [ ] >5 consecutive skips → road abandoned, pure navmesh to destination
- [ ] NavHelper's built-in FindPathAvoiding works within each segment

### Part F: Integration
- [ ] Distance >50yd + road available → RoadNavTool created
- [ ] Distance >2000yd → StrategicNavTool Walk segments use roads
- [ ] No road available → fallback to MoveToTool (current behavior)
- [ ] Bot visibly follows roads in-game (Goldshire → Stormwind test)

### In-Game Tests
- [ ] **Goldshire → Stormwind**: bot follows Elwynn Forest road
- [ ] **Darkshire → Lakeshire**: bot follows road through Redridge
- [ ] **Mob on road**: bot detours around mob, returns to road
- [ ] **No road area (Deadmines entrance)**: bot uses pure navmesh
- [ ] **Long distance (Stormwind → Southshore)**: strategic + road routing
- [ ] **Short distance (<50 yd)**: pure navmesh, no road overhead

---

## Appendix: Related Tasks

- **nav-road-mmaps.md** (Status: TODO / на подумать) — Alternative approach: modify mmaps_generator to bake road area cost into navmesh polys. Complementary to this task but **NOT a dependency**. Road Graph integration works independently of navmesh road marking.
- **nav-hostile-avoidance.md** (Status: CODE COMPLETE) — Provides the danger avoidance infrastructure (FindPathAvoiding, area cost marking, post-validation) that RoadNavTool relies on.
- **nav-navigate-tool.md** (Status: DONE) — Provides NavHelper class used for each road segment.
- **nav-humanization.md** (Status: TODO) — Movement humanization (noise, micro-pauses) applies on top of road following automatically via MovementSynth in NavHelper.
