# Task: Reverse-Engineer Gossip Quest List Structure

> **Status**: TODO
> **Created**: 2026-03-04
> **Phase**: Game Internals RE
> **Depends on**: nothing
> **Blocks**: TASK_011_quest-tools

---

## Table of Contents

1. [Goal](#goal)
2. [Problem Statement](#problem)
3. [Known Entry Points](#known-entry-points)
4. [Research Strategy](#research-strategy)
5. [Expected Deliverables](#expected-deliverables)
6. [Acceptance Criteria](#acceptance-criteria)

---

<a name="goal"></a>
## 1. Goal

Reverse-engineer the internal client-side data structure that stores gossip quest data (both available and active quests) after `GOSSIP_SHOW` fires. The primary objective is to extract **quest IDs** for quests listed in the gossip window — information that the Lua Gossip API does not expose.

Deliver a C++ function `game::quest::GetGossipQuestId(int gossipIndex, bool isAvailable) -> int` that reads quest ID directly from memory.

---

<a name="problem"></a>
## 2. Problem Statement

The WoW 3.3.5a Lua API for gossip quests does NOT return quest IDs:

- `GetGossipAvailableQuests()` → flat list of 5 values per quest: `title, level, isLowLevel, isDaily, isRepeatable`
- `GetGossipActiveQuests()` → flat list of 4 values per quest: `title, level, isLowLevel, isComplete`

Quest ID is critical for reliable identification across all Quest Tools (TASK_011). Without it, quests can only be identified by title — which is fragile and language-dependent.

**Active quests** can be cross-referenced with the quest log (`GetQuestLogTitle` returns questID as 9th value), but **available quests** (not yet accepted) have NO Lua path to quest ID.

---

<a name="known-entry-points"></a>
## 3. Known Entry Points

### 3.1 C++ Functions (build 12340)

| Function | Address | Notes |
|----------|---------|-------|
| `CGQuestInfo_C::GetNumAvailGossipQuests` | `0x0058A5D0` | Returns count of available gossip quests |
| `CGQuestInfo_C::GetNumActiveGossipQuests` | `0x0058A6C0` | Returns count of active gossip quests |
| `CGQuestInfo::IsCompletable` | `0x0058CBB0` | Checks if quest is completable |
| `GetQuestIdFromIndex` | `0x007463E0` | Reads quest ID — likely quest log, needs verification |
| `IsDailyQuest` | `0x005DECC0` | Daily quest check |

### 3.2 Lua C Handlers (reverse targets)

The Lua functions `GetGossipAvailableQuests` and `GetGossipActiveQuests` are registered C handlers. Their addresses can be found by:

1. Scanning the Lua registration table for the string `"GetGossipAvailableQuests"`
2. The registered C function reads from the internal gossip structure and pushes title/level/flags onto the Lua stack
3. Disassembling this handler reveals the gossip data structure pointer and field offsets

### 3.3 SMSG_GOSSIP_MESSAGE (Opcode 0x17D)

Server sends this packet when player interacts with NPC. The client handler parses it and populates the internal gossip structure. Finding the opcode handler for 0x17D and tracing where it stores data is an alternative RE path.

### 3.4 Object Manager Context

| Item | Value |
|------|-------|
| ClientConnection pointer | `0x00C79CE0` |
| ObjectManager offset | `+0x2ED0` |
| ClntObjMgrGetActivePlayerObj | `0x004038F0` |

---

<a name="research-strategy"></a>
## 4. Research Strategy

### Approach A: Trace Lua C handler (recommended)

1. Find the C function behind `GetGossipAvailableQuests` in the Lua registration table
2. Disassemble it — it will read from a global/static pointer to the gossip quest list
3. Identify the structure layout: `struct GossipQuestEntry { int questId; char* title; int level; int flags; ... }`
4. Identify the global pointer / accessor function that holds the array
5. Determine indexing: 0-based vs 1-based, separate arrays for available/active vs unified

### Approach B: Trace packet handler

1. Find the SMSG_GOSSIP_MESSAGE (0x17D) handler
2. Trace where parsed quest data is stored
3. Same structure discovery as Approach A

### Approach C: Trace CGQuestInfo functions

1. Disassemble `CGQuestInfo_C::GetNumAvailGossipQuests` at `0x0058A5D0`
2. It accesses the same data store — follow the pointer chain
3. Nearby functions (GetNumActive, IsCompletable) will reveal more fields

### Tools

- IDA Pro / Ghidra for static disassembly
- x32dbg for dynamic tracing (set BP on known addresses, inspect memory)
- Cheat Engine for memory scanning (open gossip → scan for quest title string → trace xrefs)

---

<a name="expected-deliverables"></a>
## 5. Expected Deliverables

### 5.1 Documentation

- Gossip quest data structure layout (fields, sizes, offsets)
- Global pointer / accessor address
- Available vs active quest storage differences (if any)

### 5.2 Code

**File: `game/quest.h`**

```cpp
namespace game::quest {

// Read quest ID from internal gossip structure.
// gossipIndex: 1-based (matching Lua convention)
// isAvailable: true = available quests, false = active quests
// Returns 0 on failure (invalid index, gossip not open)
int GetGossipQuestId(int gossipIndex, bool isAvailable);

// Read all gossip quest IDs at once
std::vector<int> GetAllGossipQuestIds(bool isAvailable);

} // namespace game::quest
```

**File: `game/quest.cpp`**
- Implementation using discovered offsets/pointers

---

<a name="acceptance-criteria"></a>
## 6. Acceptance Criteria

1. `GetGossipQuestId()` returns correct quest ID for every quest in the gossip window
2. Works for both available and active quests
3. Returns 0 gracefully when gossip is not open or index is out of range
4. Verified against multiple NPCs with different quest counts (1 quest, 3+ quests, mixed available/active)
5. Quest IDs match server-side IDs (cross-reference with quest log after accepting)
6. No crashes when called outside gossip context
