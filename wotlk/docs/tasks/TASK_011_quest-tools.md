# Task: Quest Interaction Tools

> **Status**: TODO
> **Created**: 2026-03-04
> **Phase**: Bot Tools
> **Depends on**: TASK_010_reverse-gossip-quest-structure
> **Blocks**: nothing

---

## Table of Contents

1. [Goal](#goal)
2. [Architecture Overview](#architecture)
3. [Data Layer: game::quest](#data-layer)
4. [Tool 1: QueryQuestsTool](#tool-1)
5. [Tool 2: AcceptQuestTool](#tool-2)
6. [Tool 3: QueryRewardsTool](#tool-3)
7. [Tool 4: CompleteQuestTool](#tool-4)
8. [Shared Patterns](#shared-patterns)
9. [Files Inventory](#files-inventory)
10. [Implementation Order](#implementation-order)
11. [Acceptance Criteria](#acceptance-criteria)

---

<a name="goal"></a>
## 1. Goal

Implement 4 quest interaction tools following the existing `ITool` pattern:

| # | Tool | Purpose | Type |
|---|------|---------|------|
| 1 | `QueryQuestsTool` | Get list of quests from NPC (with quest IDs) | Query (results in tool fields) |
| 2 | `AcceptQuestTool` | Accept a specific quest by quest ID | Action |
| 3 | `QueryRewardsTool` | Preview reward choices for a completable quest | Query (results in tool fields) |
| 4 | `CompleteQuestTool` | Turn in quest, with callback for reward selection | Action + Callback |

All tools embed NPC approach + interaction (like InteractTool) so they are self-contained.

---

<a name="architecture"></a>
## 2. Architecture Overview

### Design Decisions (confirmed)

1. **All 4 operations are separate ITool implementations** — not functions
2. **Quest identification: always by quest ID** — obtained from memory via TASK_010 deliverables
3. **InteractTool approach is embedded** in each tool (approach NPC + open gossip)
4. **Query results stored as fields** in the Tool — accessed via getters after `Completed`
5. **Reward selection via callback** — `CompleteQuestTool` accepts `std::function` that receives reward choices and returns the selected index

### Event-Driven Polling

All tools use frame-by-frame Lua polling (same pattern as `LootTool`):

- **GOSSIP_SHOW**: poll `GetNumGossipAvailableQuests() > 0 or GetNumGossipActiveQuests() > 0`
- **QUEST_DETAIL**: poll `GetQuestID()` — returns non-nil when detail frame is open (used by AcceptQuestTool to verify correct quest)
- **QUEST_PROGRESS / QUEST_COMPLETE**: poll via `IsQuestCompletable()` or `GetNumQuestChoices() >= 0`
- **Quest accepted**: poll quest log for questId via `GetQuestLogIndexByID(questId)`
- **Quest completed**: poll quest log — questId is no longer present

No Lua event registration needed — pure polling from `Tick()`.

### Lua API Reference (3.3.5a, build 12340)

**Gossip functions** (gossip window must be open):
- `GetNumGossipAvailableQuests()` → count
- `GetGossipAvailableQuests()` → flat list: title, level, isLowLevel, isDaily, isRepeatable (×5 per quest)
- `GetNumGossipActiveQuests()` → count
- `GetGossipActiveQuests()` → flat list: title, level, isLowLevel, isComplete (×4 per quest)
- `SelectGossipAvailableQuest(index)` — 1-based, opens QUEST_DETAIL
- `SelectGossipActiveQuest(index)` — 1-based, opens QUEST_PROGRESS

**Quest detail functions** (QUEST_DETAIL screen):
- `GetQuestID()` → quest ID of currently displayed quest
- `AcceptQuest()` → accepts the displayed quest

**Quest completion functions**:
- `CompleteQuest()` → advances from QUEST_PROGRESS to QUEST_COMPLETE reward screen (NOT finalizer)
- `GetQuestReward(choiceIndex)` → **actual finalizer**, grants rewards. 1-based choice, 0 or omit for no-choice
- `CloseQuest()` / `DeclineQuest()` → close dialog without completing

**Reward inspection** (QUEST_COMPLETE screen):
- `GetNumQuestChoices()` → count of selectable rewards
- `GetNumQuestRewards()` → count of guaranteed (non-choice) rewards
- `GetQuestItemInfo("choice", i)` → name, texture, count, quality, isUsable
- `GetQuestItemLink("choice", i)` → full item link (parse for item ID)
- `GetQuestItemInfo("reward", i)` → guaranteed reward info
- `GetQuestItemLink("reward", i)` → guaranteed reward link
- `GetRewardMoney()` → copper
- `GetRewardXP()` → experience

**Quest log**:
- `GetNumQuestLogEntries()` → (entries, quests) — entries include zone headers
- `GetQuestLogTitle(i)` → 9 values: title, level, questTag, suggestedGroup, isHeader, isCollapsed, isComplete, isDaily, questID
- `GetQuestLogIndexByID(questId)` → log index (0 if not found)

**Critical notes**:
- Booleans are `1`/`nil`, NOT `true`/`false`
- Item ID from link: `tonumber(link:match("item:(%d+)"))`
- `GetQuestItemInfo` does NOT return itemId in 3.3.5a — must parse item link
- All quest Lua functions are **unprotected** (no Lua unlocker needed)
- Must be called from **main thread only** (EndScene hook)

---

<a name="data-layer"></a>
## 3. Data Layer: `game::quest`

Shared data structures and helper functions used by all tools.

### Structures

```cpp
namespace game::quest {

struct QuestInfo {
    int         questId;
    std::string title;
    int         level;
    bool        isLowLevel;
    bool        isDaily;
    bool        isRepeatable;   // available quests only
    bool        isComplete;     // active quests only
};

struct QuestReward {
    int         index;          // 1-based
    int         itemId;         // parsed from item link
    std::string name;
    int         count;
    int         quality;        // 0=Poor, 1=Common, 2=Uncommon, 3=Rare, 4=Epic
};

} // namespace game::quest
```

### Helper Functions

```cpp
namespace game::quest {

// --- Memory read (TASK_010 deliverable) ---
int GetGossipQuestId(int gossipIndex, bool isAvailable);

// --- Lua-based readers (called from Tick on main thread) ---

// Read available quests from gossip (gossip must be open)
// Combines Lua gossip data with memory-read quest IDs
std::vector<QuestInfo> ReadGossipAvailableQuests();

// Read active quests from gossip (gossip must be open)
// Quest IDs: memory-read first, quest log cross-ref as fallback
std::vector<QuestInfo> ReadGossipActiveQuests();

// Read reward choices from QUEST_COMPLETE screen
std::vector<QuestReward> ReadRewardChoices();

// Read guaranteed (non-choice) rewards from QUEST_COMPLETE screen
std::vector<QuestReward> ReadFixedRewards();

// Read money/XP rewards
int ReadRewardMoney();
int ReadRewardXP();

// Quest log helpers
bool IsQuestInLog(int questId);
bool IsQuestCompleteInLog(int questId);

// Parse item ID from WoW item link string
int ParseItemIdFromLink(const std::string& link);

} // namespace game::quest
```

---

<a name="tool-1"></a>
## 4. Tool 1: QueryQuestsTool

**Purpose**: Approach NPC, open gossip, read all quests with IDs, complete.

### Constructor

```cpp
QueryQuestsTool(game::GUID npcGuid);
```

### Phases

```
Approaching → WaitGossip → ReadQuests → Done
```

| Phase | Logic |
|-------|-------|
| `Approaching` | Same as InteractTool: navmesh approach → CTM interact → `InteractUnit("target")`. Timeout: 15s |
| `WaitGossip` | Poll `GetNumGossipAvailableQuests()` or `GetNumGossipActiveQuests()` > 0. Timeout: 5s |
| `ReadQuests` | Call `ReadGossipAvailableQuests()` + `ReadGossipActiveQuests()`, store results. Single frame. → Done |

### Results (accessible after Completed)

```cpp
const std::vector<QuestInfo>& GetAvailableQuests() const;
const std::vector<QuestInfo>& GetActiveQuests() const;
```

### Edge Cases

- NPC has no quests (gossip opens but 0 quests) → `Completed` with empty vectors
- NPC is pure quest NPC (fires `QUEST_GREETING` instead of `GOSSIP_SHOW`) → use `GetNumAvailableQuests()` / `GetNumActiveQuests()` fallback
- NPC despawns during approach → `Failed`

---

<a name="tool-2"></a>
## 5. Tool 2: AcceptQuestTool

**Purpose**: Approach NPC, open gossip, find quest by ID, accept it.

### Constructor

```cpp
AcceptQuestTool(game::GUID npcGuid, int questId);
```

### Phases

```
Approaching → WaitGossip → FindAndSelect → WaitDetail → Accept → VerifyAccepted → Done
```

| Phase | Logic |
|-------|-------|
| `Approaching` | Same as InteractTool. Timeout: 15s |
| `WaitGossip` | Poll gossip available count > 0. Timeout: 5s |
| `FindAndSelect` | Iterate gossip quests, match quest ID via memory read (`GetGossipQuestId`). Call `SelectGossipAvailableQuest(matchedIndex)`. If not found → `Failed` |
| `WaitDetail` | Poll `GetQuestID()` — wait until it returns `m_questId`. Timeout: 3s |
| `Accept` | Call `AcceptQuest()`. Single frame |
| `VerifyAccepted` | Poll `GetQuestLogIndexByID(questId)` > 0. Timeout: 3s. If found → `Completed` |

### Edge Cases

- Quest ID not found in gossip → `Failed` with log message
- Quest has prerequisites not met (server rejects) → `VerifyAccepted` timeout → `Failed`
- Gossip window has both available and active quests → only search available list

---

<a name="tool-3"></a>
## 6. Tool 3: QueryRewardsTool

**Purpose**: Approach NPC, navigate to quest completion reward screen, read rewards, close dialog.

### Constructor

```cpp
QueryRewardsTool(game::GUID npcGuid, int questId);
```

### Phases

```
Approaching → WaitGossip → FindAndSelectActive → WaitProgress → CallComplete → WaitRewardScreen → ReadRewards → CloseDialog → Done
```

| Phase | Logic |
|-------|-------|
| `Approaching` | Same as InteractTool. Timeout: 15s |
| `WaitGossip` | Poll gossip active count > 0. Timeout: 5s |
| `FindAndSelectActive` | Match quest ID in active gossip list via memory read. Call `SelectGossipActiveQuest(matchedIndex)`. Not found → `Failed` |
| `WaitProgress` | Poll for quest progress/completion frame. Timeout: 3s |
| `CallComplete` | Call `CompleteQuest()` to advance to reward screen. Single frame |
| `WaitRewardScreen` | Poll `GetNumQuestChoices() >= 0` (reward screen present). Timeout: 3s |
| `ReadRewards` | Call `ReadRewardChoices()` + `ReadFixedRewards()` + money/XP. Store results. Single frame |
| `CloseDialog` | Call `CloseQuest()`. Single frame. → Done |

### Results (accessible after Completed)

```cpp
const std::vector<QuestReward>& GetRewardChoices() const;
const std::vector<QuestReward>& GetFixedRewards() const;
int GetRewardMoney() const;   // copper
int GetRewardXP() const;
```

### Edge Cases

- Quest not completable (objectives incomplete) → WaitProgress timeout → `Failed`
- Quest has no rewards → `Completed` with empty vectors
- No choice rewards, only fixed → `GetRewardChoices()` returns empty

### Important

This tool navigates to the reward screen and then **closes the dialog without completing**. It is a preview-only tool. To actually complete the quest, use `CompleteQuestTool`.

---

<a name="tool-4"></a>
## 7. Tool 4: CompleteQuestTool

**Purpose**: Approach NPC, navigate to completion, select reward via callback, finalize quest.

### Constructor

```cpp
using RewardCallback = std::function<int(const std::vector<QuestReward>& choices)>;

CompleteQuestTool(game::GUID npcGuid, int questId, RewardCallback callback = nullptr);
```

- `callback` receives the reward choices and returns 1-based index of the selected reward
- If `callback` is nullptr and there are choices → `Failed` (cannot auto-select)
- If there are no choices → callback is not called, proceeds with `GetQuestReward(0)`

### Phases

```
Approaching → WaitGossip → FindAndSelectActive → WaitProgress → CallComplete → WaitRewardScreen → ResolveReward → FinalizeReward → VerifyCompleted → Done
```

| Phase | Logic |
|-------|-------|
| `Approaching` | Same as InteractTool. Timeout: 15s |
| `WaitGossip` | Poll gossip active count > 0. Timeout: 5s |
| `FindAndSelectActive` | Match quest ID in active gossip list via memory read. Call `SelectGossipActiveQuest(matchedIndex)`. Not found → `Failed` |
| `WaitProgress` | Poll for quest progress frame. Timeout: 3s |
| `CallComplete` | Call `CompleteQuest()`. Single frame |
| `WaitRewardScreen` | Poll for reward screen. Timeout: 3s |
| `ResolveReward` | Read choices via `ReadRewardChoices()`. If choices > 0: invoke callback → get choice index. If choices == 0: choice = 0. If choices > 0 and no callback → `Failed`. Single frame |
| `FinalizeReward` | Call `GetQuestReward(choiceIndex)`. **Irreversible**. Single frame |
| `VerifyCompleted` | Poll `GetQuestLogIndexByID(questId)` == 0 (quest removed from log). Timeout: 3s. → `Completed` |

### Edge Cases

- Callback returns invalid index (0 or > numChoices) → `Failed`
- Quest not in active gossip list → `Failed`
- Quest objectives incomplete → `WaitProgress` timeout → `Failed`
- Server rejects completion → `VerifyCompleted` timeout → `Failed`
- NPC despawns mid-flow → `Failed`

---

<a name="shared-patterns"></a>
## 8. Shared Patterns

### 8.1 NPC Approach (embedded InteractTool logic)

All 4 tools share the same approach logic. Extract into a reusable helper or base class:

```cpp
// Option A: NavApproachHelper (composition)
class NavApproachHelper {
public:
    enum class Status { Approaching, InRange, Failed, Timeout };
    NavApproachHelper(game::GUID targetGuid, float interactRange, uint32_t timeoutMs);
    Status Tick();  // call each frame
    void Abort();
};

// Option B: QuestToolBase (inheritance)
class QuestToolBase : public ITool {
protected:
    bool TickApproach();  // returns true when gossip is open
    // ... shared approach + gossip open logic
};
```

Recommendation: **Option A (composition)** — consistent with existing `NavHelper` pattern.

### 8.2 Gossip Quest ID Resolution

All tools that search for a quest by ID in the gossip list share this logic:

```cpp
// Returns 1-based gossip index, or 0 if not found
int FindGossipIndexByQuestId(int questId, bool isAvailable);
```

Internally calls `GetGossipQuestId(i, isAvailable)` for each gossip entry.

### 8.3 Timeout Constants

```cpp
static constexpr uint32_t kApproachTimeoutMs  = 15000;
static constexpr uint32_t kGossipTimeoutMs    = 5000;
static constexpr uint32_t kDetailTimeoutMs    = 3000;
static constexpr uint32_t kProgressTimeoutMs  = 3000;
static constexpr uint32_t kRewardTimeoutMs    = 3000;
static constexpr uint32_t kVerifyTimeoutMs    = 3000;
```

### 8.4 QUEST_GREETING Fallback

Some NPCs are "pure quest NPCs" that fire `QUEST_GREETING` instead of `GOSSIP_SHOW`. These use a different API:

- `GetNumAvailableQuests()` / `GetNumActiveQuests()` (no "Gossip" prefix)
- `GetAvailableTitle(index)` / `GetActiveTitle(index)`
- `SelectAvailableQuest(index)` / `SelectActiveQuest(index)`

All tools should detect this case (gossip count == 0 but quest greeting count > 0) and switch to the appropriate API set.

---

<a name="files-inventory"></a>
## 9. Files Inventory

### New Files

| File | Purpose |
|------|---------|
| `game/quest.h` | QuestInfo, QuestReward structs + helper function declarations |
| `game/quest.cpp` | Lua-based readers, item link parser, quest log helpers |
| `bot/tools/query_quests.h` | QueryQuestsTool declaration |
| `bot/tools/query_quests.cpp` | QueryQuestsTool implementation |
| `bot/tools/accept_quest.h` | AcceptQuestTool declaration |
| `bot/tools/accept_quest.cpp` | AcceptQuestTool implementation |
| `bot/tools/query_rewards.h` | QueryRewardsTool declaration |
| `bot/tools/query_rewards.cpp` | QueryRewardsTool implementation |
| `bot/tools/complete_quest.h` | CompleteQuestTool declaration |
| `bot/tools/complete_quest.cpp` | CompleteQuestTool implementation |

### Modified Files

| File | Change |
|------|--------|
| `bot/tool.h` | Add `QueryQuests`, `AcceptQuest`, `QueryRewards`, `CompleteQuest` to `ToolType` enum |

---

<a name="implementation-order"></a>
## 10. Implementation Order

1. **`game/quest.h` + `game/quest.cpp`** — data structures + helper functions (depends on TASK_010 for `GetGossipQuestId`)
2. **`QueryQuestsTool`** — simplest tool, validates the approach + gossip + quest ID read pipeline
3. **`AcceptQuestTool`** — builds on QueryQuests flow, adds quest selection + acceptance
4. **`QueryRewardsTool`** — adds completion screen navigation + reward reading
5. **`CompleteQuestTool`** — full flow with callback + finalization

Each tool should be tested individually before moving to the next.

---

<a name="acceptance-criteria"></a>
## 11. Acceptance Criteria

### QueryQuestsTool
- [ ] Approaches NPC via navmesh/CTM
- [ ] Opens gossip window
- [ ] Returns correct quest IDs for all available quests
- [ ] Returns correct quest IDs for all active quests
- [ ] Handles QUEST_GREETING NPCs (non-gossip)
- [ ] Returns empty vectors for NPC with no quests
- [ ] Fails gracefully on timeout / NPC despawn

### AcceptQuestTool
- [ ] Finds quest by ID in gossip list
- [ ] Opens quest detail and verifies quest ID matches
- [ ] Calls AcceptQuest and verifies quest appears in log
- [ ] Fails with clear log message if quest ID not found
- [ ] Fails if quest prerequisites not met (server rejects)

### QueryRewardsTool
- [ ] Navigates to completion reward screen
- [ ] Reads all choice rewards with correct item IDs
- [ ] Reads all fixed rewards with correct item IDs
- [ ] Reads money and XP rewards
- [ ] Closes dialog without completing quest
- [ ] Fails if quest objectives incomplete

### CompleteQuestTool
- [ ] Full flow from approach to quest removal from log
- [ ] Callback receives correct reward choices
- [ ] Callback return value selects correct reward
- [ ] Handles no-choice quests without callback
- [ ] Fails if callback is null but choices exist
- [ ] Fails on invalid callback return value
- [ ] Quest is removed from log after completion
