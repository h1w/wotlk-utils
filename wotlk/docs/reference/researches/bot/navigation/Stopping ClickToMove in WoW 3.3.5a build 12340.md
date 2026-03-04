# Stopping ClickToMove in WoW 3.3.5a build 12340

**The correct way to halt CTM movement is to call `CGPlayer_C::ClickToMoveStop` at address `0x0072B3A0`.** This dedicated `__thiscall` function properly dismantles the CTM state machine, clears movement flags, and sends the server-notifying stop packet — something that writing to the CTM action field alone never does. The root cause of the user's problem is that all attempted methods (setting action to `0x0D`, `0x03`, or moving to the current position) only modify the local CTM struct without telling the movement subsystem to cease. The CMovement system has already been engaged and continues sending `MSG_MOVE_HEARTBEAT` because no `MSG_MOVE_STOP` was ever dispatched. This single function call resolves everything.

## Why writing CTM action to idle never works

The WoW client separates the CTM decision layer from the CMovement execution layer. When `CGPlayer_C::ClickToMove(action=4)` is called, the CTM system writes destination coordinates and action type into a **global static structure at `0x00CA11D8`**, then activates the CMovement subsystem, which sets the `FORWARD` movement flag (`0x1`) in the unit's `CMovementData` and sends `MSG_MOVE_START_FORWARD` (opcode **`0x00B5`**) to the server. From that point onward, the server extrapolates your position based on that start-forward notification.

Writing `0x0D` (None) or `0x13` (Idle) to the CTM action field at `CTM_Base + 0x1C` only resets the CTM scheduler. **It does not touch CMovement.** The `FORWARD` flag stays set, no `MSG_MOVE_STOP` is sent, and the server keeps extrapolating forward movement — hence the infinite `MSG_MOVE_HEARTBEAT` stream. As noted by Apoc on OwnedCore: *"There is no way to stop CTM using CTM itself, without having Lua errors being thrown."* Writing `0x03` (Stop) triggers an `AUTOFOLLOW_END` event that causes a Lua error. `MoveForwardStop()` only applies to keyboard-initiated movement and has no authority over CTM-initiated motion.

**Important correction**: The user identified `MSG_MOVE_STOP` as opcode `0x00CF`. Cross-referencing TrinityCore, AzerothCore, and WowPacketParser confirms that **`MSG_MOVE_STOP` is opcode `0x00B7`** in 3.3.5a build 12340, not `0x00CF`. The `MSG_MOVE_HEARTBEAT` opcode `0x00EE` is correct.

## The primary solution: `CGPlayer_C::ClickToMoveStop`

The function at **`0x0072B3A0`** is confirmed in the canonical OwnedCore 3.3.5.12340 Info Dump Thread, listed directly alongside `CGPlayer_C::ClickToMove` at `0x00727400`. It uses `__thiscall` convention with no parameters beyond the `this` pointer (the local player's `CGPlayer_C` object):

```cpp
// Function pointer definition
typedef void (__thiscall *tClickToMoveStop)(void* thisPlayer);
auto ClickToMoveStop = reinterpret_cast<tClickToMoveStop>(0x0072B3A0);

// Call from the game's main thread (EndScene hook, etc.)
void* localPlayer = GetLocalPlayer(); // via object manager
ClickToMoveStop(localPlayer);
```

Internally, this function reads the current CTM action from the global struct, uses a switch/jump table to handle each active state's cleanup, resets the CTM state machine to idle, **clears the `FORWARD` movement flag** in CMovementData, and **sends `MSG_MOVE_STOP`** (opcode `0x00B7`) to the server. This is the engine's own proper teardown path. It must be called from the **main game thread** — typically inside an EndScene hook, a detour from the main loop, or a Lua-registered callback. Calling from a worker thread will crash or cause undefined behavior.

## The manual fallback: flags + packet

If calling `ClickToMoveStop` is not feasible (for instance, in an out-of-process scenario or if you need more granular control), the three-step manual sequence is:

1. **Neutralize CTM**: Write `0xD` (None) to the CTM action field at `0x00CA11F4` so CTM stops re-enabling `FORWARD` each frame.
2. **Clear movement flags**: Zero out the directional bits in the unit's movement flags located at `[playerBase + 0xD8] → +0x40`. At minimum, clear bit `0x1` (FORWARD). A safe mask is `& ~0x3F` to clear all directional and turn bits.
3. **Send MSG_MOVE_STOP**: Call `CGUnit_C::SendMovementPacket` at `0x007413F0` with opcode `0x00B7`. This `__thiscall` function takes the player pointer as `this` and the opcode as its sole argument. It reads the current position and flags from CMovementData to construct the packet automatically.

```cpp
// Step 1: Kill CTM action
*(uint32_t*)0x00CA11F4 = 0xD; // CTM action = None

// Step 2: Clear movement flags
void* localPlayer = GetLocalPlayer();
uint32_t* movementDataPtr = *(uint32_t**)((uintptr_t)localPlayer + 0xD8);
uint32_t* moveFlagsPtr = (uint32_t*)((uintptr_t)movementDataPtr + 0x40);
*moveFlagsPtr &= ~0x3F; // Clear FORWARD, BACKWARD, STRAFE_L/R, TURN_L/R

// Step 3: Send stop packet
typedef void (__thiscall *tSendMovementPacket)(void* unit, uint32_t opcode);
auto SendMovementPacket = reinterpret_cast<tSendMovementPacket>(0x007413F0);
SendMovementPacket(localPlayer, 0x00B7); // MSG_MOVE_STOP
```

**Order matters.** Step 1 must come before step 2 because the CTM system runs each frame and will re-set `FORWARD` if the action is still `0x4` (Move). Step 3 must come after step 2 because `SendMovementPacket` reads the current flags when building the outgoing packet.

## Complete CTM global struct layout at `0x00CA11D8`

The CTM system uses a **global static structure** (not per-player-object) at base address **`0x00CA11D8`**. Multiple independent sources (OwnedCore info dumps, AmeisenBot, ZzukBot, WotLKRotations) confirm these offsets:

| Offset | Absolute address | Type | Field |
|--------|-----------------|------|-------|
| +0x00 | 0xCA11D8 | float | Unknown (initialization value ~6.087) |
| +0x04 | 0xCA11DC | float | Turn scale / turn speed (π radians) |
| +0x08 | 0xCA11E0 | float | Unknown |
| +0x0C | 0xCA11E4 | float | Interaction distance |
| +0x18 | 0xCA11F0 | uint32 | Timestamp |
| **+0x1C** | **0xCA11F4** | **int32** | **CTM action type** (the key field) |
| +0x20 | 0xCA11F8 | uint64 | Interact target GUID |
| +0x80 | 0xCA1258 | float | Start position X |
| +0x84 | 0xCA125C | float | Start position Y |
| +0x88 | 0xCA1260 | float | Start position Z |
| **+0x8C** | **0xCA1264** | **float** | **Destination X** |
| **+0x90** | **0xCA1268** | **float** | **Destination Y** |
| **+0x94** | **0xCA126C** | **float** | **Destination Z** |

Some references use `0xCA11E4` as an alternative base (which is `0xCA11D8 + 0x0C`). Under that convention, State is at `+0x10` and positions at `+0x80/0x84/0x88` — these resolve to the same absolute addresses. The CTM enable toggle is at `[0xBD08F4] + 0x30`.

## Movement data structure and player offsets

The `CMovementData` pointer lives at offset **`+0xD8`** from the `CGUnit_C` / `CGPlayer_C` object base. Inside that structure:

| CMovementData offset | Type | Field |
|---------------------|------|-------|
| +0x10 | float[3] | Position (X, Y, Z) |
| +0x1C | float | Heading / facing |
| +0x38 | uint64 | Transport GUID |
| **+0x40** | **uint32** | **movementFlags** (FORWARD=0x1, BACKWARD=0x2, etc.) |
| +0x44 | uint32 | movementFlags2 (extra flags) |

Direct position offsets from the player object base (no dereference needed):

| Offset | Field |
|--------|-------|
| +0x798 | Position X |
| +0x79C | Position Y |
| +0x7A0 | Position Z |
| +0x7A8 | Rotation / facing |
| +0x814 | Walk speed |
| +0x81C | Run speed |

Key movement flag values: `FORWARD=0x1`, `BACKWARD=0x2`, `STRAFE_LEFT=0x4`, `STRAFE_RIGHT=0x8`, `TURN_LEFT=0x10`, `TURN_RIGHT=0x20`, `WALK=0x100`, `SPLINE_ENABLED=0x400000`. CTM uses **client-side** movement (sets `FORWARD`, sends heartbeats), not server-side splines, so `SPLINE_ENABLED` is irrelevant here.

## Key function addresses for build 12340

| Address | Function | Relevance |
|---------|----------|-----------|
| **0x00727400** | `CGPlayer_C::ClickToMove` | Start CTM movement |
| **0x0072B3A0** | `CGPlayer_C::ClickToMoveStop` | **Stop CTM properly** |
| 0x007413F0 | `CGUnit_C::SendMovementPacket` | Send any movement opcode |
| 0x00740D30 | `CGUnit_C::OnMovementPacket` | Incoming movement handler |
| 0x00715CF0 | `GetClickToMoveStruct` | Returns CTM struct pointer |
| 0x008193B0 | `Lua_DoString` | Execute Lua code |
| 0x00819210 | `FrameScript_Execute` | Execute frame scripts |

## Conclusion

The critical insight is that the CTM action field and the CMovement subsystem are **decoupled**. The CTM struct at `0xCA11D8` is a command buffer; CMovement is the executor. Resetting the command buffer (`action = 0xD`) without telling CMovement to stop is like removing a GPS destination without hitting the brakes. **`CGPlayer_C::ClickToMoveStop` at `0x0072B3A0` is the brake pedal** — it handles the full teardown chain from CTM state reset through movement flag cleanup to server packet dispatch. For the manual approach, the critical missing step in all the user's attempts was sending `MSG_MOVE_STOP` (opcode `0x00B7`, not `0x00CF`) via `CGUnit_C::SendMovementPacket` at `0x007413F0` after clearing both the CTM action and the `FORWARD` movement flag.