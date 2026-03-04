# Quest Tools Bug Fixes — QuestApproachHelper & Turn-In Flow

**Date**: 2026-03-04

---

## Summary

Three independent bugs caused quest turn-in and reward-query tools to fail in all tested scenarios. Each bug was diagnosed via runtime log analysis and packet capture. This document covers all root causes, the investigation process, and every code change applied.

---

## Bug 1 — Stale Questgiver Data Causes False GossipOpen

### Symptom

After accepting a quest (window closes), the next `AcceptQuestTool` invocation for the same NPC would fire `GossipOpen` ~4 ms after `InteractUnit` — before the server could possibly respond — and immediately fail in `FindAndSelect` ("not found in gossip available list").

### Root Cause

The questgiver arrays at `offsets::questgiver::AvailArrayBase` / `ActiveArrayBase` and `offsets::questgiver::NpcGUID` persist after the gossip window is closed. They are only overwritten on the *next* `SMSG_QUESTGIVER_QUEST_LIST` response. Reading them immediately after `InteractUnit` returns the previous interaction's data, giving a false-positive "window is open" result.

The original code polled `GetNumAvailableQuests()` / `GetNumActiveQuests()` and `GetNumGossipAvailableQuests()` / `GetNumGossipActiveQuests()` starting from the same tick as `InteractUnit`. With stale data already in memory, the first poll instantly returned non-zero counts.

### Fix

**`quest_approach.h`** — added `kGossipMinWaitMs = 300` constant and `m_alreadyOpen` flag:

```cpp
static constexpr uint32_t kGossipMinWaitMs = 300;
bool m_alreadyOpen = false;
```

**`quest_approach.cpp`** — two changes:

1. `Init()` now detects whether the window is **already legitimately open** before moving/interacting:

```cpp
// GOSSIP_SHOW: ObjectGUID matches this NPC → gossip window already open
if (game::mem::ReadU64(offsets::gossip::ObjectGUID) == npcGuid) {
    m_useGreeting = false;
    m_alreadyOpen = true;
    return true;
}
// QUEST_GREETING: questgiver NpcGUID matches AND live counts > 0
if (game::mem::ReadU64(offsets::questgiver::NpcGUID) == npcGuid) {
    int greetAvail  = game::lua::GetInt("GetNumAvailableQuests()");
    int greetActive = game::lua::GetInt("GetNumActiveQuests()");
    if (greetAvail + greetActive > 0) {
        m_useGreeting = true;
        m_alreadyOpen = true;
        return true;
    }
}
```

If `m_alreadyOpen`, `Tick()` immediately returns `GossipOpen` — no movement or re-interaction needed.

2. `WaitGossip` phase enforces the 300 ms guard before any polling:

```cpp
if (now - m_gossipWaitStart < kGossipMinWaitMs)
    return Status::Ongoing;
```

### Why 300 ms

Measured from `InteractUnit` to observed stale-array-update: ~4 ms. Measured from `InteractUnit` to first real server response (`SMSG_QUESTGIVER_QUEST_LIST`): 200–400 ms on local/LAN. 300 ms is a conservative guard that blocks the stale window without meaningfully slowing down normal flow.

---

## Bug 2 — AutoComplete Quests Not Found in Active List

### Symptom

`CompleteQuestTool` and `QueryRewardsTool` logged "Quest not found in gossip (active or auto-complete)" for quest 5261 "Eagan Peltskinner". The tools searched only the active list (questIcon 3/4).

### Root Cause

Quest 5261 is an **auto-complete quest**. These quests appear in the gossip **available** list (questIcon != 3/4) with `autoComplete = 1`. They do not follow the standard accept→progress→reward flow. Instead:

- The quest is not in the player's log yet.
- Selecting it via `SelectGossipAvailableQuest` / `SelectAvailableQuest` triggers the detail frame.
- `CompleteAutoQuest()` accepts and immediately completes the quest in a single call.
- No reward selection screen is shown — rewards are granted silently.

The tools only searched the active list and had no fallback to the available list.

### Fix

Both `CompleteQuestTool` and `QueryRewardsTool` now fall through to the available list and check the `autoComplete` flag:

```cpp
// Not in active list — check available for auto-complete quests
idx = game::quest::FindGossipIndexByQuestId(m_questId, true, greet);
if (idx != 0) {
    bool ac = !greet
        ? game::quest::GetGossipQuestInfo(idx, true).autoComplete
        : game::mem::ReadU32(offsets::questgiver::AvailArrayBase
              + (idx - 1) * offsets::questgiver::Stride
              + offsets::questgiver::AutoComplete) != 0;
    if (ac) {
        m_isAutoComplete = true;
        if (greet)
            game::lua::Executef("SelectAvailableQuest(%d)", idx);
        else
            game::lua::Executef("SelectGossipAvailableQuest(%d)", idx);
        m_phase      = Phase::WaitProgress;
        m_phaseStart = now;
    }
}
```

**AutoComplete field location differs by window type**:

| Window type | Array | `autoComplete` field offset |
|-------------|-------|-----------------------------|
| GOSSIP_SHOW | `offsets::gossip::EntryArrayBase` (0x00BFC968) | `+0x00C` |
| QUEST_GREETING | `offsets::questgiver::AvailArrayBase` (0x00C05AE8) | `+0x20C` |

**`CallComplete` phase** — branches on `m_isAutoComplete`:

```cpp
case Phase::CallComplete: {
    if (m_isAutoComplete) {
        game::lua::Execute("CompleteAutoQuest()");
        // CompleteQuestTool: jump to VerifyCompleted (no reward screen)
        // QueryRewardsTool: set Completed immediately (rewards granted silently)
    } else {
        game::lua::Execute("CompleteQuest()");
        m_phase = Phase::WaitRewardScreen;
    }
}
```

**Files changed**: `complete_quest.h`, `complete_quest.cpp`, `query_rewards.h`, `query_rewards.cpp`.

---

## Bug 3 — Server Auto-Select Skips QUEST_GREETING

### Symptom

After fixing Bug 2, quest 5261 (now a confirmed normal turn-in quest, not auto-complete) still timed out every attempt. Diagnostic logs showed:

```
[QuestApproach] First poll (+312ms) gossipAvail=0 gossipActive=0
    gossipWindowOpen=0 gossipObjGUID=0x0 greetAvail=0 greetActive=0
[QuestApproach] Gossip wait timeout for "Eagan Peltskinner" (same zeros)
```

All values zero across the entire 5-second window, on every attempt.

### Investigation

Packet log analysis (`[SEND]` hook):

```
[SEND] #68  opcode=0x0184  size=12   ← CMSG_GOSSIP_HELLO sent (attempt 1)
[SEND] #74  opcode=0x0391  ...
[SEND] #75  opcode=0x0050  ...
              ← attempts 2 and 3: NO 0x0184 packet
[SEND] #81  opcode=0x0184  size=12   ← CMSG_GOSSIP_HELLO sent (attempt 4, ~27s later)
```

Key findings:

1. `CMSG_GOSSIP_HELLO` (0x0184) **IS sent** on the first attempt — the interaction packet reaches the server.
2. **Client throttles** `CMSG_GOSSIP_HELLO` — subsequent calls to `InteractUnit("target")` within ~15–20 seconds are silently dropped by the client without sending a packet. This means retry attempts are useless if the first interaction received no response.
3. The server **sends no response to any of the `0x0184` attempts** — neither `SMSG_GOSSIP_MESSAGE` nor `SMSG_QUESTGIVER_QUEST_LIST`.

### Root Cause

When an NPC has exactly one quest associated with the player (no available quests, one active quest), WoW 3.3.5a server **auto-selects** it and skips `SMSG_QUESTGIVER_QUEST_LIST` entirely. Instead, it responds with either:

- `SMSG_QUESTGIVER_OFFER_REWARD` — if quest objectives are complete (turn-in screen)
- `SMSG_QUESTGIVER_REQUEST_ITEMS` — if objectives are not yet complete (progress screen)

Neither of these fires `QUEST_GREETING`. The Lua APIs `GetNumActiveQuests()` and `GetNumAvailableQuests()` — which are populated by `QUEST_GREETING` — remain 0. The gossip `ObjectGUID` slot also stays 0 because `SMSG_GOSSIP_MESSAGE` was never sent.

**Both `SMSG_QUESTGIVER_OFFER_REWARD` and `SMSG_QUESTGIVER_REQUEST_ITEMS` populate `GetTitleText()`** with the quest title. This is the only signal that the server responded.

### Fix

**`quest_approach.h`** — added `m_questFrameOpen` flag and `IsQuestFrameOpen()` accessor:

```cpp
bool m_questFrameOpen = false;  // server auto-selected quest, already at offer/progress frame
bool IsQuestFrameOpen() const { return m_questFrameOpen; }
```

**`quest_approach.cpp`** — WaitGossip now also checks `GetTitleText()` as a final detection step:

```cpp
// Server may auto-select quest and skip SMSG_QUESTGIVER_QUEST_LIST.
// Both SMSG_QUESTGIVER_OFFER_REWARD and SMSG_QUESTGIVER_REQUEST_ITEMS populate GetTitleText().
std::string titleText = game::lua::GetValue("GetTitleText()");
if (!titleText.empty() && titleText != "nil") {
    m_useGreeting  = true;
    m_questFrameOpen = true;
    LOG(INFO) << "[QuestApproach] Quest frame auto-opened by server, title=\"" << titleText << "\"";
    return Status::GossipOpen;
}
```

**`complete_quest.cpp` / `query_rewards.cpp`** — `FindAndSelectActive` checks `IsQuestFrameOpen()` at the top and skips quest list navigation entirely:

```cpp
case Phase::FindAndSelectActive: {
    // Server auto-selected — already at offer-reward frame, skip quest list navigation
    if (m_approach.IsQuestFrameOpen()) {
        m_isAutoComplete = false;
        m_phase      = Phase::WaitProgress;
        m_phaseStart = now;
        break;
    }
    // ... normal quest list lookup ...
}
```

Since `GetTitleText()` is already populated when we enter `WaitProgress`, the phase transitions to `CallComplete` on the first tick. `CompleteQuest()` is then called normally.

### Flow After Fix

```
InteractUnit("target")
  → server: SMSG_QUESTGIVER_OFFER_REWARD (auto-selected)
  → WaitGossip: GetTitleText() = "Eagan Peltskinner" → m_questFrameOpen = true → GossipOpen
  → FindAndSelectActive: IsQuestFrameOpen() → skip to WaitProgress
  → WaitProgress: GetTitleText() non-nil → CallComplete (first tick)
  → CompleteQuest() → WaitRewardScreen → GetQuestReward(0) → VerifyCompleted → Done
```

---

## Bug 4 — AcceptQuestTool: Quest ID Unavailable for Verification

### Context

WoW 3.3.5a `GetQuestLogTitle(i)` returns `title, level, tag, isComplete, isHeader, isCollapsed` — **no questID** in position 7+ (that field was added in later patches). `GetQuestLogIndexByID(questId)` is not a registered Lua function in build 12340.

### Fix

`AcceptQuestTool` uses **count-based verification**: records `select(2, GetNumQuestLogEntries())` (number of non-header quest slots) before `AcceptQuest()`, then waits for it to increase:

```cpp
// Accept phase
m_questCountBefore = game::lua::GetInt("select(2,GetNumQuestLogEntries())");
game::lua::Execute("AcceptQuest()");

// VerifyAccepted phase
int currentCount = game::lua::GetInt("select(2,GetNumQuestLogEntries())");
if (currentCount > m_questCountBefore)
    m_status = ToolStatus::Completed;
```

`select(2, ...)` returns the second value of `GetNumQuestLogEntries()` — the actual quest count excluding zone headers.

---

## Questgiver RE — `offsets/questgiver.h`

### Background

`SMSG_QUESTGIVER_QUEST_LIST` (opcode `0x018D`) populates two separate arrays for available and active quests. These differ structurally from the gossip array populated by `SMSG_GOSSIP_MESSAGE`.

### Reverse Engineering

Disassembled via `disasm_questgiver.py` (capstone, same approach as gossip RE):

| Function | Address | Evidence |
|----------|---------|----------|
| `lua_GetAvailableTitle(i)` | — | `eax = (i-1)*0x214 + 0xC05AF4` → title at `AvailArrayBase+0x00C` |
| `SelectAvailableQuest_inner` | — | `eax = [idx*0x214 + 0xC05AE8]` → questId at `AvailArrayBase+0x000` |
| `lua_GetActiveTitle(i)` | — | `eax = (i-1)*0x214 + 0xC01874` → title at `ActiveArrayBase+0x00C` |
| `SelectActiveQuest_inner` | — | `eax = [idx*0x214 + 0xC01868]` → questId at `ActiveArrayBase+0x000` |

**Entry layout** (`QuestgiverEntry`, stride `0x214`):

```
+0x000  uint32_t questId
+0x004  (unknown)
+0x008  (unknown)
+0x00C  char title[512]       — null-terminated
+0x20C  uint32_t autoComplete — checked by SelectAvailableQuest before accept
+0x210  (unknown)
```

**Key difference from gossip array**: `autoComplete` is at `+0x20C` (not `+0x00C`).

**Global addresses**:

| Address | Description |
|---------|-------------|
| `0x00C05AE8` | Available quest array base |
| `0x00C0D69C` | Available quest count |
| `0x00C01868` | Active quest array base |
| `0x00C0D6A0` | Active quest count |
| `0x00C0D648` | Current questgiver NPC GUID |
| `0x00C0D448` | `GetTitleText()` string buffer |
| `0x00C0CC48` | `GetGreetingText()` string buffer |

---

## Quest Log RE — `offsets/questlog.h`

### Background

`IsQuestInLog(questId)` and `IsQuestCompleteInLog(questId)` needed to verify quest acceptance and completion without relying on Lua ID lookups (which don't exist in 3.3.5a).

### Reverse Engineering

Disassembled `inner_GetTitle` (0x5E0000) and `inner_GetIsComplete` (0x5DED30):

**Entry layout** (`QuestLogEntry`, stride `0x10`):

```
+0x00  uint32_t questId    — quest ID; zone ID for header rows
+0x04  uint32_t unk04      — slot index (internal)
+0x08  uint32_t isHeader   — non-zero = zone header (skip for quest ops)
+0x0C  uint32_t unk0C      — isCollapsed (for headers)
```

**Global addresses**:

| Address | Description |
|---------|-------------|
| `0x00C237B0` | Entry array base (up to 32 entries) |
| `0x00C23AD0` | Total entry count (incl. headers) |
| `0x00C23AE4` | Displayed count = `select(1, GetNumQuestLogEntries())` |
| `0x00C23AD8` | Currently selected quest ID |

**`GetQuestLogTitle(i)` return positions** (1-based):
`[1]=title, [2]=level, [3]=tag, [4]=isComplete, [5]=isHeader, [6]=isCollapsed`

`isComplete` values: `1` = done, `-1` = failed, `0` = in progress.

---

## Diagnostic Logging Added to QuestApproachHelper

Added to help diagnose future WaitGossip failures without code changes:

**First-poll log** (fires once at first poll after 300 ms):
```
[QuestApproach] First poll (+312ms) gossipAvail=0 gossipActive=0
    gossipWindowOpen=0 gossipObjGUID=0x0 greetAvail=0 greetActive=0
```

**Timeout log** (enhanced with full state dump):
```
[QuestApproach] Gossip wait timeout for "NPC" (gossipAvail=0 gossipActive=0
    gossipWindowOpen=0 gossipObjGUID=0x0 greetAvail=0 greetActive=0)
```

These two lines together are sufficient to distinguish:
- Server sent no response at all → all zeros
- Server responded via gossip → `gossipAvail` or `gossipActive` > 0
- Server responded via QUEST_GREETING → `greetAvail` or `greetActive` > 0
- Server auto-selected (offer/progress frame) → now caught by `GetTitleText()` before timeout

---

## Files Changed

| File | Action |
|------|--------|
| `src/bot/tools/quest_approach.h` | Modified — `m_alreadyOpen`, `m_firstPollLogged`, `m_questFrameOpen`, `IsQuestFrameOpen()`, `kGossipMinWaitMs` |
| `src/bot/tools/quest_approach.cpp` | Modified — already-open detection in `Init()`, 300 ms guard, `GetTitleText()` trigger, diagnostic logs |
| `src/bot/tools/complete_quest.h` | Modified — `m_isAutoComplete` flag |
| `src/bot/tools/complete_quest.cpp` | Modified — available-list fallback, autoComplete flow, `IsQuestFrameOpen()` skip |
| `src/bot/tools/query_rewards.h` | Modified — `m_isAutoComplete` flag |
| `src/bot/tools/query_rewards.cpp` | Modified — available-list fallback, autoComplete flow, `IsQuestFrameOpen()` skip |
| `src/bot/tools/accept_quest.cpp` | Modified — count-based `VerifyAccepted` |
| `src/offsets/questgiver.h` | **New** — questgiver array offsets (available/active arrays, `autoComplete` at `+0x20C`) |
| `src/offsets/questlog.h` | **New** — quest log entry array offsets, `GetQuestLogTitle` return positions |
| `docs/scripts/disasm_questgiver.py` | **New** — capstone disassembly of questgiver Lua handlers |
| `docs/scripts/disasm_questlog.py` | **New** — capstone disassembly of quest log internal functions |
