# Bot navigation and pathfinding in WoW 3.3.5a

Injected DLL bots for World of Warcraft 3.3.5a (build 12340) rely on a surprisingly standardized technical stack: **Recast/Detour navigation meshes identical to those used by open-source server emulators, EndScene-hooked execution on the main render thread, and the client's own ClickToMove API for packet-safe movement**. The server's movement validation in stock TrinityCore/AzerothCore is minimal — primarily physics-violation checks rather than behavioral analysis — making movement pattern humanization the primary anti-detection concern. This report covers every layer of the implementation, from navmesh generation through packet-level movement emulation, with concrete offsets, structures, and code for build 12340.

---

## 1. Navigation mesh generation from WoW's map data

All serious bot frameworks use the same navigation mesh pipeline originally built for server emulators. TrinityCore, AzerothCore, and MaNGOS generate their navmeshes offline from extracted MPQ archive data using a three-stage toolchain, and bots simply reuse these files.

**Stage 1 — map-extractor** parses `terrain.MPQ` (>1GB) and writes per-tile heightmap data into a `/maps` directory. Each ADT tile covers **533.33 yards** in a **64×64 grid** per map, with 16×16 MCNK chunks per tile, each containing 145 height vertices (9×9 outer + 8×8 inner grid).

**Stage 2 — vmap4_extractor + vmap4_assembler** extracts collision geometry from WMO (World Map Object) and M2 (doodad model) files into `/vmaps`. WMO groups provide building collision faces filtered by `WMO_MATERIAL_COLLISION` flags; M2 models provide static obstacle geometry for trees, rocks, fences, and decorative objects.

**Stage 3 — mmaps_generator (MoveMapGen)** is the core tool. It combines terrain heightmaps with vmap collision geometry and feeds everything through Recast/Detour's voxelization pipeline:

```cpp
// Per-tile build flow (TrinityCore MapBuilder.cpp):
MeshData meshData;
terrainBuilder->loadMap(mapID, tileX, tileY, meshData);   // ADT heightmap
terrainBuilder->loadVMap(mapID, tileY, tileX, meshData);  // WMO/M2 geometry
// Then the Recast pipeline:
rcCreateHeightfield → rcRasterizeTriangles → rcBuildCompactHeightfield →
rcErodeWalkableArea → rcBuildDistanceField → rcBuildRegions →
rcBuildContours → rcBuildPolyMesh → rcBuildPolyMeshDetail →
dtCreateNavMeshData → navMesh->addTile
```

Slopes are classified in two passes: triangles ≤**55°** get `NAV_AREA_GROUND`, between 55–70° get `NAV_AREA_GROUND_STEEP`, and >70° are marked unwalkable. The `rcConfig` uses `maxVertsPerPoly = 6` (Detour's constant), cell size matching `BASE_UNIT_DIM`, and `walkableRadius` of 1–2 cells depending on unit size.

Bot frameworks consume these files directly. Drew Kestell's BloogBot loads `.mmap`/`.mmtile` files via a C++ navigation DLL called through P/Invoke. AmeisenBotX runs a separate TCP navigation server process wrapping Detour. Namreeb's `namigator` library includes its own ADT/WMO parser plus Recast integration with Python bindings. The key insight is that **bots never need to build their own navmesh** — TrinityCore's `mmaps_generator` output works universally.

### MMAP file format

Each map gets a header file and per-tile data files:

```cpp
struct MmapNavMeshHeader {       // 40 bytes — one per map (e.g., 0000.mmap)
    uint32 mmapMagic;            // 0x4d4d4150 ("MMAP")
    uint32 mmapVersion;          // 16 (current)
    dtNavMeshParams params;      // origin, tileWidth, tileHeight, maxTiles, maxPolys
    uint32 offmeshConnectionCount;
};

struct MmapTileHeader {          // 20 bytes — per tile (e.g., 0003248.mmtile)
    uint32 mmapMagic;            // 0x4d4d4150
    uint32 dtVersion;            // Detour version
    uint32 mmapVersion;          // 16
    uint32 size;                 // Detour tile data blob size
    char   usesLiquids;          // boolean
    char   padding[3];
};
```

Tile filenames encode map, X, and Y coordinates: `{mapId:03d}{tileY:02d}{tileX:02d}.mmtile`. At runtime, `MMapManager::loadMap()` reads the header, validates magic/version, then calls `dtNavMesh::addTile()` with the raw Detour data blob.

### Multi-floor buildings, caves, and dungeons

Recast handles multi-floor geometry naturally through voxelization — stacked heightfield spans at different elevations produce separate walkable surfaces. The `walkableHeight` parameter in `rcBuildCompactHeightfield` determines minimum floor-to-ceiling clearance. Dungeon maps that consist entirely of WMO geometry (flagged by `wdt_uses_global_map_obj = 0x0001` in the WDT file) have no ADT terrain at all — the navmesh is built purely from WMO collision faces. Off-mesh connections handle teleporters, elevators, and disconnected surface transitions, stored per-tile with position pairs, radius, and bidirectional flags.

The critical implementation detail for multi-floor pathfinding is that `dtNavMeshQuery::findNearestPoly()` performs a 3D search — the Z (height) coordinate must be accurate to select the correct floor's polygon. A common bot bug is using stale Z values, which causes path queries to snap to the wrong floor.

### Runtime path computation

TrinityCore's `PathGenerator` class demonstrates the standard query pattern that bots replicate:

```cpp
// 1. Find start/end polygons
dtNavMeshQuery::findNearestPoly(startPos, extents, &filter, &startRef, closestStart);
dtNavMeshQuery::findNearestPoly(endPos,   extents, &filter, &endRef,   closestEnd);
// extents typically ±3.0 in XZ, ±5.0 in Y

// 2. A* through polygon graph (max 74 polygons)
dtNavMeshQuery::findPath(startRef, endRef, startPos, endPos, &filter, 
                         pathPolys, &pathCount, MAX_PATH_LENGTH);

// 3. String-pull to XYZ waypoints
dtNavMeshQuery::findStraightPath(startPos, endPos, pathPolys, pathCount,
                                  straightPath, flags, refs, &count, maxStraight);
```

The `dtQueryFilter` controls area costs and inclusion flags. Creatures that can swim include `NAV_WATER`; in-combat creatures include `NAV_GROUND_STEEP`. An empirical +0.5f Z offset is applied to all waypoints to prevent ground clipping.

---

## 2. Road-preferring pathfinding requires custom navmesh modification

**TrinityCore, AzerothCore, and MaNGOS do not implement road-preferring pathfinding.** The stock navmesh has only four area types — `GROUND` (11), `GROUND_STEEP` (10), `WATER` (9), and `MAGMA_SLIME` (8) — with no road designation. Roads in WoW are **purely visual**, encoded as texture layers rather than as distinct geometry or area flags.

Road texture data lives in the ADT's MCLY (texture layer) chunks, where each of up to 4 layers references a texture filename via the MTEX chunk (e.g., `Tileset/Elwynn/ElwynnDirt01.blp`). The `effectId` field links to `GroundEffectTexture.dbc`, which in turn references `TerrainType.dbc` for footstep sounds — road surfaces produce distinct sounds (cobblestone, dirt path) compared to grass or forest floor.

Three approaches exist for implementing road preference:

**Texture-based area marking** requires modifying `mmaps_generator` to parse MCLY/MTEX, identify road textures by filename pattern (`*Dirt*`, `*Road*`, `*Cobble*`), assign a custom `NAV_AREA_ROAD` (e.g., area ID 12), and set a lower `dtQueryFilter::setAreaCost()` for that area (0.5 vs 1.0 for regular ground). This produces the most natural results but demands custom tooling.

**TerrainType sound heuristic** samples the terrain type at positions along candidate paths using the `GroundEffectTexture.dbc → TerrainType.dbc` chain, biasing toward road terrain types. This works at runtime without navmesh modification but is imprecise.

**Waypoint overlay** is the simplest approach and the one most bots actually use: pre-recorded waypoint paths along major roads are overlaid on navmesh pathfinding, with the navmesh used only for deviation recovery and off-road segments. Many grinding/questing bots simply hardcode road waypoints for common routes.

When Detour area costs are used, the mechanism is straightforward: `filter.setAreaCost(NAV_AREA_ROAD, 0.5f)` makes road polygons half the traversal cost, causing A* to strongly prefer road paths even when off-road shortcuts are geometrically shorter.

---

## 3. Obstacle avoidance via TraceLine and collision detection

### The TraceLine function (build 12340)

WoW's client-side raycasting function lives at address **`0x007A3B70`** with a wrapper (`CGWorldFrame::Intersect`) at `0x007A3BE0`:

```c
// Address: 0x007A3B70, calling convention: __cdecl
typedef uint8_t (__cdecl* TraceLine_t)(
    Vector3& end,          // ray destination
    Vector3& start,        // ray origin
    Vector3& hitPoint,     // output: intersection point
    float&   hitDistance,   // output: fraction [0.0–1.0]
    uint32_t hitFlags,     // CGWorldFrameHitFlags bitmask
    uint32_t optional      // usually 0
);
// Returns 1 if ray hit something (LoS blocked), 0 if clear
```

Note that parameter order is **end before start** — a common source of bugs. The wrapper at `0x007A3BE0` does not clean the stack (caller must `add esp, 0x18`), while the core function at `0x007A3B70` is stdcall and self-cleans.

### Collision hit flags

```csharp
[Flags]
enum CGWorldFrameHitFlags : uint {
    HitTestBoundingModels  = 0x00000001,  // M2 models (trees, rocks, fences)
    HitTestWMO             = 0x00000010,  // Buildings, structures
    HitTestGround          = 0x00000100,  // Terrain
    HitTestLiquid          = 0x00010000,  // Water, lava
    HitTestMovableObjects  = 0x00100000,  // Elevators, doors
    // Composites:
    HitTestLOS = 0x100011,               // Standard line-of-sight check
    HitTestAll = 0x100171,               // Full collision check
}
```

Bots use TraceLine for several purposes: **LoS verification** before engaging targets, **path segment validation** by casting rays between consecutive navmesh waypoints to eliminate unnecessary intermediate points, **ground height sampling** by casting vertical rays downward, and **underground node detection** (if a downward ray from an herb/ore position hits ground above the node, it's inside terrain). For obstacle avoidance specifically, bots cast multiple forward-facing rays at small angular offsets to detect obstacles not captured in the navmesh — a technique adapted from Reynolds-style steering behaviors.

### Dynamic obstacle avoidance

Dynamic entities (other players, moving NPCs) aren't in the navmesh and require runtime handling. Bots poll the ObjectManager each tick to get positions of nearby units, calculate distances, and apply steering forces. The simplest approach is separation steering — push the path laterally away from nearby entities proportional to proximity. More sophisticated bots implement reciprocal velocity obstacles (RVO) for smooth avoidance of moving entities while maintaining general path direction.

---

## 4. Hostile NPC detection and threat-aware routing

### ObjectManager traversal (build 12340)

The ObjectManager linked list is the foundation for all environmental awareness:

```csharp
// Core traversal pattern
uint clientConn   = Read<uint>(0x00C79CE0);           // Static client connection
uint objMgr       = Read<uint>(clientConn + 0x2ED0);   // ObjectManager offset
ulong localGuid   = Read<ulong>(objMgr + 0xC0);        // Local player GUID
uint currentObj    = Read<uint>(objMgr + 0xAC);         // First object in list

while (currentObj != 0 && (currentObj & 1) == 0) {
    uint type = Read<uint>(currentObj + 0x14);          // Object type
    // Type 3 = Unit/NPC, Type 4 = Player
    currentObj = Read<uint>(currentObj + 0x3C);         // Next object
}
```

Key unit data offsets from the object base pointer: position at **`+0x798/0x79C/0x7A0`** (X/Y/Z), facing at `+0x7A8`, and the descriptor array pointer at `+0x08`. From the descriptor array: `UNIT_FIELD_LEVEL` at `+0x88`, `UNIT_FIELD_FACTIONTEMPLATE` at `+0x8C`, `UNIT_FIELD_HEALTH` at `+0x58`, `UNIT_FIELD_FLAGS` at `+0xB8`.

### Hostility determination

Two methods exist. **Injected bots** call `GetUnitReaction` at **`0x006061E0`** (__thiscall), which returns an enum: Hated (1), Hostile (2), Unfriendly (3), Neutral (4), Friendly (5–8). **Out-of-process bots** read `UNIT_FIELD_FACTIONTEMPLATE` and check `FactionTemplate.dbc` (pointer at `0x00A37340`), comparing enemy/friend group bitmasks.

### Aggro range calculation

TrinityCore's `Creature::GetAttackDistance` defines the formula bots replicate:

```
aggroRadius = 20.0 - combatReach + (creatureLevel - playerLevel)
clamped to [5, 45] yards
```

An equal-level mob aggros at roughly **20 yards**. Each level the mob exceeds the player adds 1 yard; each level below subtracts 1 yard. Bots typically add a **3–5 yard safety margin** and treat the result as a circular avoidance zone centered on each hostile NPC.

### Integration into pathfinding

The standard pipeline works as follows: enumerate ObjectManager → filter for hostile units (reaction ≤ 2) with health > 0 → calculate each unit's danger radius → either mark navmesh polygons within danger zones as high-cost before querying Detour, or post-process the Detour path by inserting detour waypoints around zones that intersect path segments. The ObjectManager is re-scanned every **100–500ms**, triggering path recalculation when new hostiles appear on the current path or existing ones move significantly.

---

## 5. Movement packets and human-like emulation

### Packet structure

All movement packets share a `MovementInfo` payload. Every packet includes a **packed GUID**, followed by:

```
uint32  movementFlags     // See flags enum below
uint16  extraFlags        // Extra movement flags
uint32  timestamp         // Client time in milliseconds
float   x, y, z           // Position
float   orientation       // Facing [0, 2π] radians
uint32  fallTime          // Always present
// Conditional fields for transport, swimming/flying pitch, jumping, spline
```

Key movement opcodes (MSG prefix indicates bidirectional):

| Opcode | Hex | Purpose |
|--------|-----|---------|
| MSG_MOVE_START_FORWARD | 0x0B5 | Begin forward movement |
| MSG_MOVE_STOP | 0x0B7 | Stop all movement |
| MSG_MOVE_JUMP | 0x0BB | Initiate jump |
| MSG_MOVE_START_TURN_LEFT/RIGHT | 0x0BC/0x0BD | Begin turning |
| MSG_MOVE_SET_FACING | 0x0DA | Update orientation |
| MSG_MOVE_HEARTBEAT | 0x0EE | Periodic position update (~500ms) |
| MSG_MOVE_START_ASCEND | 0x359 | Begin flying upward |

The movement flags enum defines the state machine:

```cpp
MOVEMENTFLAG_FORWARD    = 0x00000001,  MOVEMENTFLAG_BACKWARD   = 0x00000002,
MOVEMENTFLAG_STRAFE_L   = 0x00000004,  MOVEMENTFLAG_STRAFE_R   = 0x00000008,
MOVEMENTFLAG_TURN_LEFT  = 0x00000010,  MOVEMENTFLAG_TURN_RIGHT = 0x00000020,
MOVEMENTFLAG_WALKING    = 0x00000100,  MOVEMENTFLAG_ONTRANSPORT= 0x00000200,
MOVEMENTFLAG_FALLING    = 0x00001000,  MOVEMENTFLAG_SWIMMING   = 0x00200000,
MOVEMENTFLAG_FLYING     = 0x02000000,  MOVEMENTFLAG_HOVER      = 0x20000000,
```

### ClickToMove generates indistinguishable packets

This is the single most important implementation detail for bot movement. When `CGPlayer_C__ClickToMove` (at **`0x727400`**) is invoked, the game engine internally handles rotation, acceleration, and all packet generation. The server receives normal `MSG_MOVE_START_FORWARD`, `MSG_MOVE_HEARTBEAT`, and `MSG_MOVE_STOP` packets — **identical to manual keyboard/mouse movement**. CTM is the safest movement method because the game client itself produces all the network traffic.

```cpp
// Function signature:
BOOL __thiscall CGPlayer_C__ClickToMove(
    WoWActivePlayer* this,      // ECX via thiscall
    CLICKTOMOVETYPE  clickType, // 0x4 = Move
    WGUID*           guid,      // NULL for position moves
    WOWPOS*          clickPos,  // {x, y, z}
    float            precision  // stop distance
);
```

The CTM global struct at **`0x00CA11D8`** can also be written directly: destination at `+0x8C/0x90/0x94` (X/Y/Z), action type at `+0x1C`, interaction distance at `+0x0C`. The struct must be "activated" once per session by writing `1` to `[*(0xBD08F4) + 0x30]`.

### Facing and camera rotation

**The character's facing orientation IS sent to the server** in every movement packet's `orientation` field and via `MSG_MOVE_SET_FACING` (0x0DA). However, **camera angle is a purely client-side concept** — only character facing (the direction the character model points) is transmitted. A bot that moves but never sends facing updates or turn packets (`MSG_MOVE_START_TURN_LEFT/RIGHT`) for extended periods creates a detectable anomaly. Bots should periodically inject `MSG_MOVE_SET_FACING` packets with gradually changing orientation values, especially when changing direction along a path.

### Path smoothing and micro-deviations

Straight-line navmesh paths are the primary behavioral signature of bot movement. Several techniques address this:

- **Bezier curve interpolation**: Replace sharp waypoint-to-waypoint segments with quadratic or cubic Bezier curves using control points offset slightly from the direct path. AmeisenNavigation supports Chaikin Curve, Catmull-Rom Spline, and Bezier smoothing modes.
- **Perlin noise displacement**: Apply low-amplitude Perlin noise (typically **0.1–0.5 game units**) to X/Y coordinates during movement to simulate human input imprecision.
- **Random waypoint offsets**: Offset each intermediate waypoint by a small Gaussian random vector (σ ≈ **0.3–1.0 yards**) to prevent pixel-perfect path repetition across runs.
- **Stopping imprecision**: Don't stop at exact waypoint coordinates — overshoot or undershoot by **0.5–2.0 yards** and include a brief post-stop orientation adjustment.

### Jumping patterns

Human players jump sporadically while running, with a long-tail distribution — sometimes no jumps for minutes, sometimes in clusters. Bots should use a **Poisson-like distribution** with a mean interval of roughly **10–30 seconds**, with variance. Perfectly periodic jumps at fixed intervals are a strong bot signature. Jumps are implemented by sending `MSG_MOVE_JUMP` (0x0BB) with proper fall time tracking, and the server does track jump state transitions for multi-jump detection.

### Speed variation

Bots cannot vary actual movement speed without triggering speed hack detection. Instead, they simulate variable pace through **micro-pauses** (50–200ms stops every 30–120 seconds), occasional **run/walk toggles** (`MSG_MOVE_SET_RUN_MODE` / `MSG_MOVE_SET_WALK_MODE`), and slight delays between CTM waypoint advances. The key is introducing temporal variance without exceeding the server's allowed speed rate.

---

## 6. Server-side detection is physics-focused, not behavioral

### Stock TrinityCore validation

The `MovementHandler` in stock TrinityCore performs surprisingly minimal checks:

- **Coordinate validity**: `Trinity::IsValidMapCoord()` rejects NaN or out-of-range values
- **Below-map check**: Triggers fall-to-void damage if Z < map minimum height
- **Transport bounds**: Transport-relative positions must be within ±75.0 on all axes
- **Mover GUID validation**: Packed GUID must match expected mover
- **Teleport state**: Ignores packets during `IsBeingTeleported()`
- **Sit-to-stand**: Auto-stands if sitting with movement/turning flags

**Notably absent**: path straightness analysis, packet timing regularity checks, orientation change frequency analysis, or any behavioral heuristics. The server is fundamentally **client-authoritative** for movement — it trusts movement packets with minimal verification.

### AzerothCore mod-anticheat

The community `mod-anticheat` module adds physics-violation detection:

```
SpeedHack:     actualDistance > timeDelta × serverMaxSpeed × (1 + tolerance%)
FlyHack:       MOVEMENTFLAG_CAN_FLY without flight aura
JumpHack:      Multiple jumps without landing (flag analysis)
TeleportHack:  Position delta exceeds maximum possible distance
ClimbHack:     Unnatural Z-axis changes during ground movement
WaterWalkHack: MOVEMENTFLAG_WATERWALKING without corresponding aura
```

The speed check formula is: `allowedDistance = timeDelta × serverMaxSpeed × (1.0 + SpeedLimitTolerance/100)`. When actual distance exceeds this, the player is flagged. Configuration thresholds determine escalation from warnings to auto-jail, setback, kick, or ban.

### What isn't detected

**No open-source server core implements behavioral bot detection.** Path straightness, heartbeat timing regularity, orientation change patterns, jump interval distributions, and stopping precision analysis are not present in TrinityCore, AzerothCore, or MaNGOS anticheat code. This means the primary detection risk for well-implemented bots comes from Warden (client-side scanning) and GM observation, not server-side movement analysis.

The detectable bot signatures that a human GM would notice: constant speed with zero variation, perfectly straight inter-waypoint segments, regular heartbeat timing, absence of facing changes, metronomic jump intervals, and machine-precision stops at exact coordinates.

---

## 7. Implementation architecture of injected DLL bots

### EndScene hook as the execution backbone

All injected DLL bots hook DirectX 9's `EndScene` function (vtable index **42** of `IDirect3DDevice9`). The D3D device pointer for build 12340 lives at `*(*(0xC5DF88) + 0x397C)`. This hook fires every rendered frame (~16ms at 60fps), providing a reliable main-thread execution point — critical because WoW's Lua engine and most game functions are not thread-safe.

The typical bootstrap sequence: an external injector loads a C++ DLL into WoW's process via `CreateRemoteThread`. This DLL optionally initializes the .NET CLR (for C# bots), hooks EndScene, and launches the bot assembly. Each EndScene call triggers the bot's main tick/pulse function, which reads game state, evaluates the behavior state machine, and issues CTM commands or Lua calls.

### Navigation thread management

**Pathfinding runs on a separate thread** in all well-architected bots, because Detour queries can take several milliseconds — too long for a per-frame EndScene callback. Three patterns exist:

**In-process navigation DLL** (BloogBot pattern): A C++ DLL exports `CalculatePath(mapId, start, end, smooth, &length)` called via P/Invoke. The bot's worker thread queries Detour; results are queued and consumed on the main thread during EndScene.

**TCP navigation server** (AmeisenBotX pattern): A separate process loads TrinityCore MMAPs and serves path requests over TCP. The bot sends `(mapId, startPos, endPos)` and receives a waypoint array. Benefits include memory isolation, multi-instance support, and independent restart capability.

**HTTP API server** (WowNav pattern): Similar to TCP but using REST endpoints, allowing any language client to request paths.

### Path execution pipeline

```
1. Bot determines target destination
2. Request path from navigation system (separate thread/process)
3. Receive Vector3[] waypoint array
4. For each waypoint:
   a. Call CGPlayer_C__ClickToMove(Move, NULL, &waypoint, 1.0f)
   b. Each EndScene tick: check distance to current waypoint
   c. When within ~1–3 yards, advance to next waypoint
5. On arrival, trigger next behavioral state
```

Path recalculation triggers: stuck detection (distance traveled per N ticks below threshold), target movement beyond ~5–10 yards from last path endpoint, entering combat, or new obstacle detection. Stuck recovery typically involves jumping, brief reversal, lateral strafe, then full path recalculation with a random lateral offset on the next waypoint.

### Architectural diagram

```
┌──────────────────── WoW.exe Process ────────────────────┐
│                                                          │
│  D3D9 Device          Bot Logic (C#/.NET)               │
│  EndScene()──hook──→  OnPulse() {                       │
│  (vtable[42])           read ObjectManager (0xC79CE0)   │
│                         evaluate state machine           │
│  CTM Struct             write CTM / call 0x727400       │
│  0x00CA11D8             process nav results              │
│  Action/Pos/GUID      }                                 │
│                                                          │
│  TraceLine             Bootstrap DLL (C++)              │
│  0x007A3B70            init .NET CLR, hook EndScene     │
└────────────────────────┬─────────────────────────────────┘
                         │ TCP/HTTP
              ┌──────────▼──────────┐
              │  Navigation Server   │
              │  dtNavMesh + MMAPs   │
              │  findPath()          │
              │  findStraightPath()  │
              └─────────────────────┘
```

---

## 8. Open-source references and community resources

### GitHub repositories

| Repository | Language | Key Feature |
|---|---|---|
| **Jnnshschl/AmeisenBotX** | C# | Full bot for 12340 with behavior trees, EndScene hook, TCP nav server |
| **Jnnshschl/AmeisenNavigation** | C++ | TCP nav server using TC MMAPs + Recast/Detour, multiple smoothing modes |
| **namreeb/namigator** | C++ | Navigation library with own ADT/WMO parser, supports Alpha through WotLK |
| **namreeb/wowreeb** | C++ | Multi-version launcher with DLL injection, loads native + .NET assemblies |
| **Helix-Development/WowNav** | C#/C++ | HTTP REST navigation API with .NET 6 client library |
| **Zz9uk3/WoW-3.3.5a-Bot** | C# | Bot for 12340 by Zzuk, referenced in BloogBot tutorials |
| **AzDeltaQQ/WotLKRotations** | Python/C++ | Python + injected DLL framework with Named Pipe IPC |
| **TrinityCore/TrinityCore** | C++ | Server emulator with `mmaps_generator`, `PathGenerator`, `MMapManager` |
| **azerothcore/mod-anticheat** | C++ | Community anticheat module with speed/fly/jump/teleport detection |

### Key learning resources

Drew Kestell's **BloogBot tutorial** (drewkestell.us) provides a complete 20+ chapter walkthrough from C# DLL injection through EndScene hooking, ObjectManager reading, Lua integration, and Recast/Detour navigation — the single best end-to-end reference. Michael Cutler's **CapekNav** articles cover navigation mesh theory and implementation with WoW-specific examples. The **OwnedCore Info Dump Thread** for 3.3.5.12340 remains the canonical offset reference, with multi-page community-verified addresses for CTM, ObjectManager, TraceLine, Lua functions, aura structures, and descriptor fields. The **wowdev.wiki** provides authoritative documentation of file formats (ADT, WMO, WDT), the ClickToMove structure across versions, and opcode enumerations.

## Conclusion

The technical landscape of WoW 3.3.5a bot navigation reveals a mature, well-understood domain where the hard problems are largely solved. Recast/Detour provides production-quality pathfinding that bots share with the server emulators themselves. ClickToMove at `0x727400` generates network-indistinguishable movement packets, eliminating an entire class of detection vectors. The real frontier is behavioral: stock server cores perform no path-pattern analysis whatsoever, but human GMs can spot the telltale signs of robotic movement — perfect straightness, metronomic timing, absent facing changes, and machine-precision stopping. The most sophisticated bots therefore invest heavily in stochastic humanization: Bezier-smoothed paths with Perlin noise displacement, Poisson-distributed jump timing, graduated stopping imprecision, and periodic facing updates that simulate a wandering camera. The gap between "functional" and "undetectable" lies entirely in these behavioral details.