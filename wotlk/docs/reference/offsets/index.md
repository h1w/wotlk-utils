# WoW 3.3.5a (build 12340) — Offsets Reference

> Centralized offset documentation for WoW client reverse engineering.
> C++ headers: `wotlk/src/offsets/`

---

## Quick Reference — Hooked Functions

| Address | Function | Convention | File | C++ Const |
|---------|----------|-----------|------|-----------|
| `0x00819210` | FrameScript_Execute | `__cdecl` | [scripting.md](functions/scripting.md) | `offsets::FrameScript_Execute` |
| `0x007DA850` | WardenHandler | naked | [warden.md](functions/warden.md) | `offsets::WardenHandler` |
| `0x00632B50` | SendPacket | naked | [network.md](functions/network.md) | `offsets::SendPacket` |
| `0x00774EA0` | ARC4::Process | `__thiscall` | [encryption.md](functions/encryption.md) | `offsets::ARC4_Process` |

---

## Directory Structure

```
offsets/
├── index.md                          ← you are here
│
├── functions/                        — Function addresses
│   ├── scripting.md                  — FrameScript, Lua engine (33)
│   ├── lua_api.md                    — lua_* registered API (2,143)
│   ├── network.md                    — SendPacket, CDataStore, sockets (225)
│   ├── encryption.md                 — ARC4, RC4, SHA1, MD5 (14)
│   ├── warden.md                     — Warden handler, anti-cheat (6)
│   ├── object_manager.md             — Object/GUID/inventory ops (132)
│   ├── movement.md                   — ClickToMove, splines, physics (173)
│   ├── combat.md                     — Combat log, threat (14)
│   ├── spell.md                      — SpellCast, auras, effects (66)
│   ├── ui.md                         — Frames, widgets, rendering (77)
│   ├── world.md                      — Camera, terrain, M2/WMO (68)
│   ├── chat.md                       — Chat channels, messages (11)
│   ├── crt_internal.md               — CRT runtime, __crt*, __sbh* (557)
│   └── misc.md                       — CVar, console, timers, other (1,087)
│
├── packet_handlers/                  — Packet handler functions
│   ├── smsg.md                       — Server→client handlers (258)
│   ├── cmsg.md                       — Client→server (note)
│   └── msg.md                        — Bidirectional handlers (22)
│
├── data_structures/                  — Memory layouts and enums
│   ├── object_fields.md              — OBJECT/ITEM/UNIT/PLAYER descriptors
│   ├── cdatastore.md                 — CDataStore layout (+0x04=buffer, etc.)
│   ├── vtable.md                     — VTable indices
│   └── guid.md                       — GUID format, ObjectType enum
│
└── globals/                          — Static addresses and constants
    ├── dbc.md                        — ClientDB enums + addresses (239)
    └── static_addresses.md           — Global vars, camera, collision, D3D9
```

---

## File Descriptions

### functions/

| File | Description | Entries |
|------|-------------|---------|
| [scripting.md](functions/scripting.md) | FrameScript execution, Lua engine integration | 33 |
| [lua_api.md](functions/lua_api.md) | All `lua_*` C implementations of WoW's Lua API | 2,143 |
| [network.md](functions/network.md) | Packet send/recv, CDataStore operations, client services | 225 |
| [encryption.md](functions/encryption.md) | ARC4 session cipher, RC4, SHA1, MD5, HMAC | 14 |
| [warden.md](functions/warden.md) | Warden anti-cheat handler, module operations | 6 |
| [object_manager.md](functions/object_manager.md) | Object creation, GUID handling, inventory, bags | 132 |
| [movement.md](functions/movement.md) | ClickToMove, splines, vehicles, swim/fly/jump | 173 |
| [combat.md](functions/combat.md) | Combat log entries, threat, attack animations | 14 |
| [spell.md](functions/spell.md) | Spell casting, auras, effects, spell DB | 66 |
| [ui.md](functions/ui.md) | UI frames, widgets, fonts, textures, models | 77 |
| [world.md](functions/world.md) | Camera, terrain, maps, M2/WMO collision | 68 |
| [chat.md](functions/chat.md) | Chat channels, messages, gossip | 11 |
| [crt_internal.md](functions/crt_internal.md) | CRT runtime internals (__crt*, malloc, etc.) | 557 |
| [misc.md](functions/misc.md) | CVars, console, debug, timers, uncategorized | 1,087 |

### packet_handlers/

| File | Description | Entries |
|------|-------------|---------|
| [smsg.md](packet_handlers/smsg.md) | Server→client message handlers | 258 |
| [cmsg.md](packet_handlers/cmsg.md) | Client→server (not present in client binary) | — |
| [msg.md](packet_handlers/msg.md) | Bidirectional message handlers | 22 |

### data_structures/

| File | Description |
|------|-------------|
| [object_fields.md](data_structures/object_fields.md) | Descriptor field enums: Object, Item, Unit, Player, etc. |
| [cdatastore.md](data_structures/cdatastore.md) | CDataStore memory layout, packet format, header encryption |
| [vtable.md](data_structures/vtable.md) | Virtual function table indices per class |
| [guid.md](data_structures/guid.md) | 64-bit GUID format, ObjectType enum, High GUID values |

### globals/

| File | Description | Entries |
|------|-------------|---------|
| [dbc.md](globals/dbc.md) | Client database (DBC) enum indices + memory addresses | 239 |
| [static_addresses.md](globals/static_addresses.md) | Global variables: camera, collision, D3D9, ObjectManager, etc. | 100+ |

---

## C++ Integration

Headers: `wotlk/src/offsets/`

```cpp
#include "offsets/offsets.h"

// Use as:
auto addr = offsets::FrameScript_Execute;  // 0x00819210
auto opc  = offsets::CMSG_WARDEN_DATA;     // 0x2E7
```

---

## Source

Original raw dump preserved at: [`offsets.txt`](../offsets.txt)
