#pragma once
// =============================================================================
// Questgiver quest data structure offsets — WoW 3.3.5a (build 12340)
//
// Reverse-engineered via capstone disassembly of questgiver Lua C handlers.
// Scripts: wotlk/docs/scripts/disasm_questgiver.py
//
// These arrays are populated by SMSG_QUESTGIVER_QUEST_LIST (opcode 0x18D),
// sent when an NPC opens QUEST_GREETING (pure quest NPC with no gossip menu).
// Unlike gossip quests, these arrays are indexed separately for available/active.
//
// AVAILABLE quest array:
//   Base (entry 0):  0x00C05AE8       — questId of first available quest
//   Count:           0x00C0D69C       — uint32_t number of available quests
//
// ACTIVE quest array:
//   Base (entry 0):  0x00C01868       — questId of first active quest
//   Count:           0x00C0D6A0       — uint32_t number of active quests
//
// Entry layout (QuestgiverEntry, stride 0x214):
//   +0x000  uint32_t questId     — quest ID
//   +0x004  (unknown)            — possibly level/flags
//   +0x008  (unknown)
//   +0x00C  char     title[512]  — null-terminated title string (+0x200 bytes)
//   +0x20C  uint32_t autoComplete— checked by SelectAvailableQuest before accept
//   +0x210  (unknown)
// Total: 0x214 bytes (532)
//
// Evidence:
//   lua_GetAvailableTitle(i): eax = (i-1)*0x214 + 0xC05AF4  → title string at +0x00C
//   SelectAvailableQuest_inner: eax = [idx*0x214 + 0xC05AE8] → questId at +0x000
//   lua_GetActiveTitle(i):   eax = (i-1)*0x214 + 0xC01874  → title string at +0x00C
//   SelectActiveQuest_inner: eax = [idx*0x214 + 0xC01868] → questId at +0x000
// =============================================================================

#include <cstdint>
#include <cstddef>

namespace offsets::questgiver {

// --- Available quests ---

// QuestgiverEntry[N] available array, entry 0 base
inline constexpr uintptr_t AvailArrayBase  = 0x00C05AE8;

// Number of available quests (uint32_t)
inline constexpr uintptr_t AvailCount      = 0x00C0D69C;

// --- Active quests ---

// QuestgiverEntry[N] active array, entry 0 base
inline constexpr uintptr_t ActiveArrayBase = 0x00C01868;

// Number of active quests (uint32_t)
inline constexpr uintptr_t ActiveCount     = 0x00C0D6A0;

// --- Entry layout (offsets from entry base) ---

inline constexpr uintptr_t QuestId         = 0x000;   // uint32_t
inline constexpr uintptr_t Title           = 0x00C;   // char[512]
inline constexpr uintptr_t AutoComplete    = 0x20C;   // uint32_t

// Stride between entries
inline constexpr size_t    Stride          = 0x214;

// Maximum entries per array (practical limit; array is count-terminated)
inline constexpr size_t    MaxEntries      = 32;

// --- Other questgiver state ---

// Current quest-detail title text (string buffer for GetTitleText())
inline constexpr uintptr_t TitleTextBuf    = 0x00C0D448;

// Greeting text buffer (for GetGreetingText())
inline constexpr uintptr_t GreetingTextBuf = 0x00C0CC48;

// NPC GUID for current questgiver interaction (lo+hi at +0/+4)
inline constexpr uintptr_t NpcGUID         = 0x00C0D648;  // uint64_t

// Pending quest interaction flag (set=1 by Select*Quest, cleared on response)
inline constexpr uintptr_t PendingFlag     = 0x00C0D6AC;  // uint32_t

} // namespace offsets::questgiver
