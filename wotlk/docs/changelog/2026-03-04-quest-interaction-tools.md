# Quest Interaction Tools (TASK_011)

**Date**: 2026-03-04

---

## Summary

Implemented 4 quest interaction bot tools (`QueryQuestsTool`, `AcceptQuestTool`, `QueryRewardsTool`, `CompleteQuestTool`) following the `ITool` pattern. Extended `game::quest` with Lua-based quest readers and created a shared `QuestApproachHelper` to encapsulate the NPC approach + gossip-open phases common to all tools.

> **See also**: [`2026-03-04-quest-tools-bugfixes.md`](2026-03-04-quest-tools-bugfixes.md) — follow-up bugfixes covering stale questgiver data, auto-complete quest detection, server auto-select behavior, and the questgiver/questlog RE that supported them.

---

## Changes

### 1. Modified: `wotlk/src/game/quest.h`

Extended with new structures and helper function declarations for TASK_011:

- `QuestInfo` struct — combined Lua + memory quest data (questId, title, level, isLowLevel, isDaily, isRepeatable, isComplete)
- `QuestReward` struct — reward item data (index, itemId, name, count, quality)
- `ReadGossipAvailableQuests(bool useGreeting)` — reads available quests from gossip or QUEST_GREETING window
- `ReadGossipActiveQuests(bool useGreeting)` — reads active quests; for QUEST_GREETING, cross-references quest log for IDs
- `ReadRewardChoices()` / `ReadFixedRewards()` — reads QUEST_COMPLETE reward screen items
- `ReadRewardMoney()` / `ReadRewardXP()` — reads copper and XP reward values
- `IsQuestInLog(int questId)` / `IsQuestCompleteInLog(int questId)` — quest log polling helpers
- `IsGossipWindowOpen()` — checks gossip NPC GUID slot at `0x00C016F0`
- `FindGossipIndexByQuestId(int questId, bool isAvailable, bool useGreeting)` — returns 1-based gossip index for a quest ID
- `ParseItemIdFromLink(const std::string& link)` — extracts item ID from WoW item link string

### 2. Modified: `wotlk/src/game/quest.cpp`

Added implementations for all new helper functions:

- Added `#include "lua_bridge.h"` for Lua API access
- `ReadGossipAvailableQuests`: gossip mode stores flat list into `_wqt` table (5 values × N); greeting mode reads per-quest via `GetAvailableTitle(i)`
- `ReadGossipActiveQuests`: gossip mode reads flat list (4 values × N); greeting mode cross-references quest log by title to recover quest IDs
- `ReadRewardChoices` / `ReadFixedRewards`: uses `GetQuestItemInfo` + `GetQuestItemLink`; item IDs parsed from links
- `IsGossipWindowOpen`: reads `offsets::gossip::ObjectGUID` directly
- `FindGossipIndexByQuestId`: normal gossip — memory read; QUEST_GREETING active — quest log cross-ref; QUEST_GREETING available — returns 0 (no ID available)
- `ParseItemIdFromLink`: `std::atoi` on string after "item:" prefix

### 3. Modified: `wotlk/src/bot/tool.h`

Added 4 new `ToolType` enum values:

```cpp
QueryQuests,
AcceptQuest,
QueryRewards,
CompleteQuest,
```

### 4. New: `wotlk/src/bot/tools/quest_approach.h` + `quest_approach.cpp`

Shared NPC approach + gossip-open helper used by all 4 tools (composition pattern):

```
Status: Ongoing | GossipOpen | Failed
```

- `Init(npcGuid)`: resolves NPC name/pos, starts navmesh or direct CTM
- `Tick()`: handles Approaching phase (navmesh → CTM → InteractUnit) + WaitGossip phase
  - WaitGossip checks gossip API first, then QUEST_GREETING API, then raw GUID slot
  - Sets `m_useGreeting` flag for the tool to use the correct select/count API set
- Timeouts: 15s approach, 5s gossip wait

### 5. New: `wotlk/src/bot/tools/query_quests.h` + `query_quests.cpp`

`QueryQuestsTool(game::GUID npcGuid)` — reads all quests from NPC gossip window.

Phases: `Approaching → ReadQuests → Done`

Result accessors (valid after `Completed`):
```cpp
GetAvailableQuests() → vector<QuestInfo>
GetActiveQuests()    → vector<QuestInfo>
```

### 6. New: `wotlk/src/bot/tools/accept_quest.h` + `accept_quest.cpp`

`AcceptQuestTool(game::GUID npcGuid, int questId)` — finds and accepts a specific quest.

Phases: `Approaching → FindAndSelect → WaitDetail → Accept → VerifyAccepted`

- `FindAndSelect`: uses `FindGossipIndexByQuestId` + `SelectGossipAvailableQuest`
- `WaitDetail`: polls `GetQuestID()` until it returns the expected quest ID (3s timeout)
- `VerifyAccepted`: polls `GetQuestLogIndexByID` until quest appears in log (3s timeout)

### 7. New: `wotlk/src/bot/tools/query_rewards.h` + `query_rewards.cpp`

`QueryRewardsTool(game::GUID npcGuid, int questId)` — preview reward choices WITHOUT completing.

Phases: `Approaching → FindAndSelectActive → WaitProgress → CallComplete → WaitRewardScreen → ReadRewards → CloseDialog`

- Calls `CloseQuest()` after reading — dialog is closed, quest remains in log
- Result accessors: `GetRewardChoices()`, `GetFixedRewards()`, `GetRewardMoney()`, `GetRewardXP()`

### 8. New: `wotlk/src/bot/tools/complete_quest.h` + `complete_quest.cpp`

`CompleteQuestTool(game::GUID npcGuid, int questId, RewardCallback callback)` — full completion flow.

```cpp
using RewardCallback = std::function<int(const std::vector<QuestReward>& choices)>;
```

Phases: `Approaching → FindAndSelectActive → WaitProgress → CallComplete → WaitRewardScreen → ResolveReward → FinalizeReward → VerifyCompleted`

- `ResolveReward`: invokes callback; validates 1-based choice index
- `FinalizeReward`: calls `GetQuestReward(choiceIndex)` — **irreversible**
- `VerifyCompleted`: polls until quest leaves log (3s timeout)
- Abort: calls `CloseQuest()` only if `FinalizeReward` has not yet been reached

### 9. Modified: `wotlk/wotlk.vcxproj`

Added all new files to the project (ClInclude + ClCompile entries).

### 10. Modified: `wotlk/docs/tasks/TASK_011_quest-tools.md`

Status changed: `TODO` → `DONE`.

---

## Technical Notes

### QUEST_GREETING detection

Some NPCs open `QUEST_GREETING` instead of `GOSSIP_SHOW` (pure quest NPCs with no gossip options). `QuestApproachHelper` detects this by checking both Lua API sets each frame during WaitGossip. The `UseGreeting()` flag is propagated to all quest reads and `FindGossipIndexByQuestId`.

### Quest IDs for QUEST_GREETING available quests

For QUEST_GREETING NPCs, the gossip entry array at `0x00BFC968` is NOT populated (server sends `SMSG_QUESTGIVER_QUEST_LIST` instead of `SMSG_GOSSIP_MESSAGE`). Available quest IDs cannot be recovered — they are returned as 0. `AcceptQuestTool` targeting a QUEST_GREETING NPC will fail on `FindAndSelect` for available quests by design.

### Active quest IDs via quest log cross-reference

For QUEST_GREETING active quests, IDs are recovered by matching `GetActiveTitle(i)` against `GetQuestLogTitle(j)` and reading questID from the 9th return value. This cross-reference is O(n×m) but practical (n≤5, m≤25).

---

## Files Changed

| File | Action |
|------|--------|
| `src/game/quest.h` | Modified — QuestInfo, QuestReward structs + helper declarations |
| `src/game/quest.cpp` | Modified — Lua-based readers, quest log helpers, search helpers |
| `src/bot/tool.h` | Modified — 4 new ToolType enum values |
| `src/bot/tools/quest_approach.h` | **New** — QuestApproachHelper declaration |
| `src/bot/tools/quest_approach.cpp` | **New** — QuestApproachHelper implementation |
| `src/bot/tools/query_quests.h` | **New** — QueryQuestsTool declaration |
| `src/bot/tools/query_quests.cpp` | **New** — QueryQuestsTool implementation |
| `src/bot/tools/accept_quest.h` | **New** — AcceptQuestTool declaration |
| `src/bot/tools/accept_quest.cpp` | **New** — AcceptQuestTool implementation |
| `src/bot/tools/query_rewards.h` | **New** — QueryRewardsTool declaration |
| `src/bot/tools/query_rewards.cpp` | **New** — QueryRewardsTool implementation |
| `src/bot/tools/complete_quest.h` | **New** — CompleteQuestTool declaration |
| `src/bot/tools/complete_quest.cpp` | **New** — CompleteQuestTool implementation |
| `wotlk.vcxproj` | Modified — all new files registered |
| `docs/tasks/TASK_011_quest-tools.md` | Modified — Status: TODO → DONE |
