# WoW 3.3.5a quest interaction: Lua API and memory reference

**Every quest-related Lua function in 3.3.5a (build 12340) is unprotected**, meaning `AcceptQuest()`, `CompleteQuest()`, `GetQuestReward()`, and all `SelectGossip*` calls execute freely through `FrameScript::Execute` at `0x00819210` without a hardware-event bypass or Lua unlocker. The full quest lifecycle—listing, accepting, inspecting rewards, and turning in—maps to a well-documented state machine driven by server-acknowledged events (`GOSSIP_SHOW`, `QUEST_DETAIL`, `QUEST_PROGRESS`, `QUEST_COMPLETE`). Chaining calls without waiting for these events is the single most common failure mode. This guide covers every Lua function signature, the complete event-driven flow, all known C++ addresses for build 12340, and the practical anti-cheat landscape.

---

## Gossip-based quest enumeration from NPCs

Two fundamentally different NPC types exist. **Gossip NPCs** fire `GOSSIP_SHOW` and expose the `GetGossip*` family. **Pure quest NPCs** (no gossip frame) fire `QUEST_GREETING` when they offer multiple quests, or jump straight to `QUEST_DETAIL` / `QUEST_PROGRESS` for a single quest. All gossip functions return valid data **only while the gossip frame is open** (after `GOSSIP_SHOW` fires).

### Available quests (pickup)

`GetNumGossipAvailableQuests()` returns a single number: the count of quests the player can pick up. `GetGossipAvailableQuests()` returns a **flat list of 5 values per quest** (not a table):

```
title1, level1, isLowLevel1, isDaily1, isRepeatable1,
title2, level2, isLowLevel2, isDaily2, isRepeatable2, ... 
```

- `title` — string, quest name
- `level` — number (can be **-1** for profession/special quests)
- `isLowLevel` — **1 or nil** (trivial/grey quest). In 3.3.5, booleans are `1/nil`, not `true/false`
- `isDaily` — 1/nil
- `isRepeatable` — 1/nil

Fields like `isLegendary` (added 5.0.4), `isIgnored` (7.0.3), and `questID` (8.2.5) **do not exist in 3.3.5**. Iterate using `select()`:

```lua
local n = GetNumGossipAvailableQuests()
for i = 1, n do
  local title, level, isLow, isDaily, isRepeat = select(i*5 - 4, GetGossipAvailableQuests())
end
```

### Active quests (turn-in candidates)

`GetNumGossipActiveQuests()` returns the count. `GetGossipActiveQuests()` returns **4 values per quest**:

```
title1, level1, isLowLevel1, isComplete1, ...
```

The critical field is `isComplete` (**1** = objectives met, ready to turn in; **nil** = still in progress). This is how you distinguish completable turn-ins from in-progress quests that merely belong to this NPC.

### Non-gossip NPCs (QUEST_GREETING context)

When a pure quest NPC has multiple quests, use a separate API family:

- `GetNumAvailableQuests()` / `GetNumActiveQuests()`
- `GetAvailableTitle(index)` / `GetActiveTitle(index)` — 1-based
- `SelectAvailableQuest(index)` / `SelectActiveQuest(index)` — 1-based

These fire after `QUEST_GREETING` and mirror the gossip functions but without the flat-list return convention.

---

## Accepting a quest: the full event-driven flow

Quest acceptance is a **three-step state machine**: open dialog → view details → accept. Each step requires waiting for a server-acknowledged event before proceeding.

**Gossip NPC flow:**
1. Interact with NPC → `GOSSIP_SHOW` fires
2. Call `SelectGossipAvailableQuest(index)` (index is **1-based**) → `QUEST_DETAIL` fires
3. Call `AcceptQuest()` → quest enters log
4. Events: `QUEST_ACCEPTED(questLogIndex, questID)` → `QUEST_LOG_UPDATE` → `QUEST_FINISHED`

**Pure quest NPC (single quest):**
1. Interact → `QUEST_DETAIL` fires directly (no selection step)
2. Call `AcceptQuest()`
3. Same events as above

**Pure quest NPC (multiple quests):**
1. Interact → `QUEST_GREETING` fires
2. Call `SelectAvailableQuest(index)` → `QUEST_DETAIL` fires
3. Call `AcceptQuest()`

`AcceptQuest()` does nothing if called before `QUEST_DETAIL` fires. A common edge case: quests with `QUEST_FLAGS_AUTO_ACCEPT` are accepted on interaction—no `AcceptQuest()` call needed. Quests flagging PvP may require `ConfirmAcceptQuest()` instead.

---

## Quest reward inspection functions

All reward functions operate on the **currently displayed quest dialog** (the one whose `QUEST_COMPLETE` event fired most recently). They are not quest-log functions.

| Function | Returns | Notes |
|---|---|---|
| `GetNumQuestChoices()` | number | Count of **chooseable** rewards |
| `GetNumQuestRewards()` | number | Count of **guaranteed** (always-given) rewards |
| `GetQuestItemInfo(type, idx)` | name, texture, count, quality, isUsable | `type` = `"choice"`, `"reward"`, or `"required"`; idx is **1-based** |
| `GetQuestItemLink(type, idx)` | itemLink string | Full `\|Hitem:...\|h` link |
| `GetRewardMoney()` | number | Copper (divide: `gold = floor(c/10000)`) |
| `GetRewardXP()` | number | XP reward including bonuses |

**Extracting item IDs** from the 3.3.5 link format `|cFFFFFFFF|Hitem:ITEMID:ENCHANT:GEM1:GEM2:GEM3:0:SUFFIX:UNIQUE:LEVEL|h[Name]|h|r`:

```lua
local itemID = tonumber(GetQuestItemLink("choice", 1):match("item:(%d+)"))
```

Note that `GetQuestItemInfo` does **not** return `itemID` directly in 3.3.5 (that was added later); parsing the link is the only reliable method. For quest-log rewards (outside the NPC dialog), use `GetQuestLogChoiceInfo()` and `GetQuestLogRewardInfo()` instead.

---

## Completing and turning in a quest

Turn-in is a **four-step state machine**, and the naming is counterintuitive: `CompleteQuest()` does **not** finalize the quest—it advances from the progress screen to the reward-selection screen. `GetQuestReward()` is the actual finalizer.

**Full gossip NPC turn-in flow:**
1. Interact → `GOSSIP_SHOW` fires
2. Call `SelectGossipActiveQuest(index)` (1-based) → `QUEST_PROGRESS` fires
3. Check `IsQuestCompletable()` — returns true if objectives are met
4. Call `CompleteQuest()` → `QUEST_COMPLETE` fires (reward selection screen)
5. Enumerate rewards: `GetNumQuestChoices()`, `GetQuestItemInfo("choice", i)`
6. Call **`GetQuestReward(choiceIndex)`** — finalizes completion, irreversible
7. Events: `QUEST_FINISHED` → `QUEST_LOG_UPDATE`

**GetQuestReward indexing:** For quests with chooseable rewards, pass the **1-based index** of the desired reward (1 to `GetNumQuestChoices()`). For quests with **no choice rewards**, pass **0** or omit the argument entirely. The default Blizzard UI uses `QuestFrameRewardPanel.itemChoice` which defaults to 0.

**Non-gossip NPC turn-in (single quest):** interaction fires `QUEST_PROGRESS` directly—skip to step 3.

**Non-gossip NPC turn-in (multiple quests):** interaction fires `QUEST_GREETING` → call `SelectActiveQuest(index)` → `QUEST_PROGRESS` → continue from step 3.

---

## Quest log API for tracking quest state

`GetNumQuestLogEntries()` returns two values: `numEntries` (includes zone headers; **affected by collapsed state**) and `numQuests` (actual quest count, always accurate). Maximum quests in 3.3.5: **25**.

`GetQuestLogTitle(questLogIndex)` returns **9 values** in 3.3.5:

```
title, level, questTag, suggestedGroup, isHeader, isCollapsed, isComplete, isDaily, questID
```

Key fields: `isHeader` (1/nil — skip headers when iterating), `isComplete` (**+1** = done, **-1** = failed, **nil** = in progress), `questID` (the Wowhead-style ID, added in patch 3.3.0 and available in 3.3.5).

`GetQuestLogIndexByID(questID)` converts a quest ID to a log index—added in 3.3.0, confirmed available. `IsQuestComplete(questID)` likely does **not exist** in 3.3.5 (added later); use `GetQuestLogTitle` iteration instead:

```lua
for i = 1, GetNumQuestLogEntries() do
  local title, _, _, _, isHeader, _, isComplete, _, qID = GetQuestLogTitle(i)
  if not isHeader and qID == targetQuestID then
    return isComplete == 1
  end
end
```

---

## Build 12340 memory addresses and C++ functions

All addresses are absolute virtual addresses for the unrebased `Wow.exe` (base `0x00400000`). ASLR is **not active** in 3.3.5a. Sources: OwnedCore Info Dump Thread, DrGonzo/TOM_RUS offset dumps, GitHub repos (tomrus88/WowAddin, johnmoore/WoW-Object-Manager).

### Core execution and Lua engine

| Function | Address | Notes |
|---|---|---|
| **FrameScript::Execute** | **0x00819210** | `__cdecl(const char* code, const char* name, bool tainted)`. Push 0 for tainted. |
| FrameScript_GetText | 0x00819D40 | Retrieve Lua variable values post-execution |
| **GetLocalizedText** | **0x007225E0** | `__thiscall`, player ptr in ECX |
| FrameScript_RegisterFunction | 0x004181B0 | Register custom C→Lua functions |
| FrameScript_SignalEvent | 0x0081AC90 | Dispatch Lua events |
| **lua_State\* (global)** | **0x00D3F78C** | Pointer to WoW's embedded Lua 5.1.4 state |
| lua_pcall | 0x0084EC50 | Direct Lua C API alternative to FrameScript |
| luaL_loadbuffer | 0x0084F860 | Load Lua chunk without FrameScript |

### NPC interaction via vtable

**`CGUnit_C::OnRightClick`** lives at **vtable index 44 (byte offset 0xB0)**, resolved address **0x00731260**. This is the programmatic equivalent of right-clicking an NPC:

```cpp
typedef void (__thiscall *InteractFn)(void* thisObj);
void* vtable = *(void**)unitPtr;
InteractFn interact = ((InteractFn*)vtable)[44];
interact(unitPtr);  // Must be called from main thread
```

Supporting functions: `CGPlayer_C::CanInteract` at **0x00729530** (distance/flag check), `CGGameUI::CloseInteraction` at `0x00512E60`, and `CGGameObject_C::OnRightClick` at **0x00712F30** for game objects.

### Object manager traversal

| Item | Value |
|---|---|
| **ClientConnection pointer** | **0x00C79CE0** |
| ObjectManager offset | +0x2ED0 from ClientConnection |
| First object | +0xAC from ObjMgr |
| Next object | +0x3C from current object |
| Local player GUID | +0xC0 from ObjMgr |
| Object type | +0x14 from object base |
| Object GUID | +0x30 from object base (8 bytes) |
| Descriptor base (m_storage) | +0x08 from object base |
| **ClntObjMgrGetActivePlayer** | **0x004D3790** (returns GUID) |
| **ClntObjMgrGetActivePlayerObj** | **0x004038F0** (returns CGPlayer_C\*) |
| **EnumVisibleObjects** | **0x004D4B30** |

### Position offsets (relative to object base)

Unit X/Y/Z: **+0x798, +0x79C, +0x7A0**. Facing: +0x7A8. Movement field pointer: +0xD8.

### Quest-specific C++ functions

| Function | Address |
|---|---|
| CGQuestInfo_C::GetNumActiveGossipQuests | 0x0058A6C0 |
| CGQuestInfo_C::GetNumAvailGossipQuests | 0x0058A5D0 |
| CGQuestInfo::IsCompletable | 0x0058CBB0 |
| GetQuestIdFromIndex | 0x007463E0 |
| IsDailyQuest | 0x005DECC0 |

### Player quest log in descriptors

Quest data lives in player descriptors starting at field index **0x00A6** (`PLAYER_QUEST_LOG_1_1`). Each of the 25 quest slots occupies **5 DWORDs** (20 bytes): Quest ID, state bitfield, and three objective counters. Read quest ID for slot N: `*(uint32_t*)(descriptorBase + (0xA6 + (N-1)*5) * 4)`.

### Other key globals

| Item | Address |
|---|---|
| LocalPlayerGUID (static) | 0x00BD07A8 |
| LastHardwareAction timestamp | 0x00B4999C |
| Direct3D9 Device pointer | 0x00C5DF88 |
| EndScene offset chain | Device → +0x397C → +0xA8 |

---

## Protection, Warden, and server-side validation

### Quest Lua functions are entirely unprotected

The taint/secure execution system in 3.3.5 marks **movement, combat, and targeting** functions as PROTECTED or HW-event-required (e.g., `CastSpellByName`, `MoveForwardStart`, `TargetUnit`). **No quest interaction function carries protection**. `AcceptQuest()`, `CompleteQuest()`, `GetQuestReward()`, `SelectGossipAvailableQuest()`, and all related calls are freely callable from FrameScript::Execute without any Lua unlocker patch. Players routinely use `/run AcceptQuest()` macros, confirming this.

For functions that *are* HW-protected (irrelevant to quests but useful context): the internal check at `0x00494A57` inspects a hardware-event flag. Patching this address is a **known Warden scan target**—avoid it unless needed for non-quest automation.

### Server-side validation on TrinityCore/AzerothCore

The server independently validates every quest operation. Key checks from TrinityCore source:

- **Proximity**: `GetNPCIfCanInteractWith(guid, UNIT_NPC_FLAG_QUESTGIVER)` enforces **~5–10 yard** interaction range on every CMSG. Being out of range silently drops the request.
- **Prerequisites**: Quest accept validates level, class, race, required chain completion, and that the NPC actually offers the quest (`object->hasQuest(questId)`).
- **Objective completion**: Turn-in validates all kill counts, collected items, and explored areas before granting rewards.
- **Reward validity**: Invalid reward choice indices trigger server-side logging of possible packet hacking.
- **Sequence independence**: The server does **not** strictly require opening gossip before sending `CMSG_QUESTGIVER_ACCEPT_QUEST`—each handler validates the NPC independently. However, the client needs the dialog events to populate Lua state.

Standard TrinityCore/AzerothCore does **not** rate-limit quest opcodes, though some servers add custom anti-flood measures.

### Warden on private servers

Warden is implemented in TrinityCore/AzerothCore but its effectiveness depends entirely on the server's `warden_checks` database table. Many private servers have **incomplete signature databases** or disable Warden entirely. The default tables ship with limited entries. Warden check types include `MEM_CHECK` (memory integrity), `PAGE_CHECK_B` (PE header scanning for injected DLLs), `MODULE_CHECK` (known DLL hashes), and `LUA_EVAL_CHECK` (arbitrary Lua evaluation on the client).

For quest-only automation (no Lua unlocker needed), the primary Warden risk is **DLL injection detection** via `PAGE_CHECK_B` and `MODULE_CHECK`. Mitigations: compile a unique DLL (won't match known hashes), avoid patching scanned memory addresses, and consider manual-mapping the DLL to avoid PE header exposure.

---

## Pitfalls, timing, and practical architecture

### Thread safety is non-negotiable

`FrameScript::Execute` and all object-manager access **must** occur on the main game thread. The standard pattern hooks `EndScene` (reachable through the D3D9 device at `0x00C5DF88 → +0x397C → vtable[42]`) and queues Lua calls for execution during the hook. Calling from any other thread—including `DllMain/DLL_PROCESS_ATTACH`—causes access violations against the object manager.

### Event-driven state machine, not sequential calls

The correct architecture registers a hidden frame for events and advances a state machine:

```lua
local f = CreateFrame("Frame")
f:RegisterEvent("GOSSIP_SHOW")
f:RegisterEvent("QUEST_DETAIL")
f:RegisterEvent("QUEST_PROGRESS")
f:RegisterEvent("QUEST_COMPLETE")
f:RegisterEvent("QUEST_FINISHED")
f:SetScript("OnEvent", function(self, event, ...)
  -- advance state machine based on event
end)
```

Each quest action involves a network round-trip (**50–200ms** typical). Calling `AcceptQuest()` and `CompleteQuest()` in the same frame will silently fail because the server hasn't acknowledged the accept yet. Wait for each event before proceeding.

### Edge cases that break naive implementations

- **Stale GUIDs**: NPCs can despawn or phase between gossip-open and quest-accept. Always re-validate.
- **Quest items not immediately queryable**: After accepting a quest, `GetContainerItemQuestInfo()` may need one frame delay to return accurate data as the client cache populates.
- **UI reloads**: A `/reload` destroys all frames and event registrations. The bot must detect `PLAYER_ENTERING_WORLD` and reinitialize.
- **Auto-accept quests**: Some quests with `QUEST_FLAGS_AUTO_ACCEPT` enter the log on interaction alone—calling `AcceptQuest()` afterward is harmless but unnecessary.
- **GetNumQuestLogEntries caveat**: The first return value counts collapsed headers; use the **second** return for actual quest count.

## Conclusion

Quest automation in 3.3.5a is architecturally straightforward because Blizzard never protected the quest Lua API—the entire accept/complete lifecycle works through `FrameScript::Execute` without any memory patching. The critical design constraint is **event-driven timing**: every action must wait for its corresponding server-acknowledged event before the next step. For programmatic NPC interaction, the vtable call at index 44 (`0x00731260`) replaces right-clicking, while the object manager rooted at `0x00C79CE0 → +0x2ED0` provides NPC enumeration and distance checking. On private servers, Warden is typically weak or disabled, making DLL injection the lowest-risk vector—but the server still validates proximity, prerequisites, and objective completion on every quest opcode, so client-side automation cannot bypass game logic, only the UI.