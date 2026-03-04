# Gossip Quest Structure RE (TASK_010)

**Date**: 2026-03-04

---

## Summary

Reverse-engineered the internal client-side gossip quest data structure (`CGQuestInfo`) via static disassembly of `Wow.exe` using Python + capstone. Implemented `game::quest` — a C++ module that reads quest IDs directly from the gossip entry array, bypassing the Lua API limitation (Lua `GetGossipAvailableQuests` does not expose quest IDs in 3.3.5a).

---

## Background

`TASK_011_quest-tools` requires quest IDs to operate. The Lua API in 3.3.5a does not return quest IDs from gossip functions — `questID` was added to `GetGossipAvailableQuests()` only in patch 8.2.5. For available quests (not yet in the quest log) there is no Lua path to the quest ID whatsoever. Direct memory reading is required.

---

## Changes

### 1. Reverse Engineering — gossip quest data structure

**Method**: static disassembly via `capstone` Python library on `Wow.exe` (build 12340).

**Functions disassembled**:

| Address | Function |
|---------|----------|
| `0x0058A550` | `SetGossipObjectGUID` |
| `0x0058A5D0` | `CGQuestInfo_C::GetNumAvailGossipQuests` |
| `0x0058A660` | `CGQuestInfo_C::GetAvailableQuestInfoFromIndex` |
| `0x0058A6C0` | `CGQuestInfo_C::GetNumActiveGossipQuests` |
| `0x0058A750` | `CGQuestInfo_C::GetActiveQuestFromIndex` |
| `0x0058A7B0` | `ClearGossipQuests` |
| `0x0058B1B0` | `Packet_SMSG_GOSSIP_MESSAGE` |
| `0x0058B3A0` | `lua_GetGossipAvailableQuests` |
| `0x0058B490` | `lua_GetGossipActiveQuests` |

**Discovered structure**:

```
GossipQuestEntry  (0x214 bytes, 32 slots at 0x00BFC968)
  +0x000  uint32_t questId       — quest ID (0 = unused)
  +0x004  int32_t  questLevel    — quest level (-1 = special)
  +0x008  uint32_t questFlags    — 0x1000=daily, 0x0040=repeatable
  +0x00C  uint32_t autoComplete  — from packet byte
  +0x010  uint32_t questIcon     — 3/4 = active, other = available
  +0x014  char     title[0x200]  — inline null-terminated string
```

**Key findings**:
- Available and active quests live in the **same flat array** — no separate storage
- Discrimination is by `questIcon`: values 3/4 → active; all others → available
- First entry with `questId == 0` marks end of valid data (zero-terminated)
- `GetAvailableQuestInfoFromIndex(idx0)` returns `0xBFC968 + idx * 0x214` (pointer to entry)
- The found-path at `0x58A6A7` confirms: `imul eax, eax, 0x214; add eax, 0xBFC968`

**Global state**:

| Address | Description |
|---------|-------------|
| `0x00BFC968` | `GossipQuestEntry[32]` — entry array base |
| `0x00C016F0` | Gossip NPC object GUID (uint64) |
| `0x00C016F8` | Gossip menu text ID (uint32) |
| `0x00C016FC` | Gossip option count (uint32) |

### 2. New: `wotlk/src/offsets/gossip.h`

New offsets header with all gossip-related addresses and field offsets. Included via `offsets.h`.

```cpp
namespace offsets::gossip {
    EntryArrayBase = 0x00BFC968
    EntryStride    = 0x214
    MaxEntries     = 32
    // field offsets: QuestId, QuestLevel, QuestFlags, AutoComplete, QuestIcon, Title
    // function addresses: GetNumAvail, GetAvailByIdx, GetNumActive, GetActiveByIdx, ...
}
```

### 3. New: `wotlk/src/game/quest.h` + `quest.cpp`

New game SDK module implementing quest ID extraction:

```cpp
namespace game::quest {
    // Core — quest ID by 1-based gossip index (matches Lua convention)
    int  GetGossipQuestId(int gossipIndex, bool isAvailable);
    int  GetGossipQuestCount(bool isAvailable);
    std::vector<int> GetAllGossipQuestIds(bool isAvailable);

    // Extended — full entry metadata
    GossipQuestInfo              GetGossipQuestInfo(int gossipIndex, bool isAvailable);
    std::vector<GossipQuestInfo> GetAllGossipQuestInfos(bool isAvailable);
}

struct GossipQuestInfo {
    int questId, questLevel;
    bool isDaily, isRepeatable, autoComplete;
    std::string title;
};
```

Implementation walks the global entry array using SEH-safe `game::mem::ReadU32` reads. Returns 0/empty safely when gossip is not open.

### 4. New: analysis scripts

- `wotlk/docs/scripts/disasm_gossip.py` — disassembles all CGQuestInfo gossip functions, extracts global address references
- `wotlk/docs/scripts/disasm_gossip2.py` — extended analysis: found-path return values, ClearGossipQuests, ActiveQuestLookup

### 5. New: research documentation

- `wotlk/docs/reference/researches/bot/quests/gossip_quest_struct.md` — full RE writeup with struct layout, disassembly evidence, usage examples

### 6. Modified: `wotlk/src/offsets/offsets.h`

Added `#include "gossip.h"` — gossip offsets are now part of the main offsets header.

### 7. Modified: `wotlk/docs/tasks/TASK_010_reverse-gossip-quest-structure.md`

Status changed: `TODO` → `DONE`.

---

## Technical Notes

### Why the Lua API is insufficient

`GetGossipAvailableQuests()` in 3.3.5a pushes `title, level, isLowLevel, isDaily, isRepeatable` per quest — no ID. The `questID` return value was added in 8.2.5. Active quests can be cross-referenced via `GetQuestLogTitle` (which does return questID), but available quests have zero Lua path to their ID.

### Why direct memory reading is safe

The gossip entry array is populated by `SMSG_GOSSIP_MESSAGE` and cleared by `ClearGossipQuests`. It's valid for the entire duration a gossip window is open. All reads are SEH-protected via `game::mem::ReadU32` — any access violation returns 0 gracefully.

---

## Files Changed

| File | Action |
|------|--------|
| `src/offsets/gossip.h` | **New** — gossip entry array offsets, field offsets, function addresses |
| `src/offsets/offsets.h` | Modified — added `#include "gossip.h"` |
| `src/game/quest.h` | **New** — `game::quest` module API |
| `src/game/quest.cpp` | **New** — `game::quest` module implementation |
| `docs/reference/researches/bot/quests/gossip_quest_struct.md` | **New** — RE research document |
| `docs/scripts/disasm_gossip.py` | **New** — capstone disassembly script |
| `docs/scripts/disasm_gossip2.py` | **New** — extended disassembly (found paths, ClearGossipQuests) |
| `docs/tasks/TASK_010_reverse-gossip-quest-structure.md` | Modified — Status: TODO → DONE |
