#pragma once
// =============================================================================
// Gossip quest data structure offsets — WoW 3.3.5a (build 12340)
//
// Reverse-engineered via capstone disassembly of CGQuestInfo functions.
// Research doc: wotlk/docs/reference/researches/bot/quests/gossip_quest_struct.md
//
// Global quest entry array:
//   Base:    0x00BFC968
//   Stride:  0x214 (532 bytes per entry)
//   Entries: 32 max
//
// Entry layout (GossipQuestEntry):
//   +0x000  uint32_t questId        — quest ID (0 = slot unused)
//   +0x004  int32_t  questLevel     — quest level (-1 = profession/special)
//   +0x008  uint32_t questFlags     — quest flags (0x1000 = daily)
//   +0x00C  uint32_t autoComplete   — from packet byte, 0 or 1
//   +0x010  uint32_t questIcon      — 3/4 = active quest, other = available
//   +0x014  char     title[0x200]   — inline null-terminated string (512 bytes)
// Total: 0x214 bytes
//
// questIcon discrimination (confirmed from GetNumAvail/GetNumActive):
//   available:  questIcon != 3 && questIcon != 4
//   active:     questIcon == 3 || questIcon == 4
// =============================================================================

#include <cstdint>
#include <cstddef>

namespace offsets::gossip {

// --- Global state ---

// GossipQuestEntry[32] array — populated by SMSG_GOSSIP_MESSAGE (0x0058B1B0)
inline constexpr uintptr_t EntryArrayBase  = 0x00BFC968;

// Gossip object GUID (NPC being talked to): lo at +0, hi at +4
inline constexpr uintptr_t ObjectGUID      = 0x00C016F0;  // uint64_t

// Menu/text ID from SMSG_GOSSIP_MESSAGE
inline constexpr uintptr_t TextId          = 0x00C016F8;  // uint32_t

// Number of gossip options (text buttons, not quests)
inline constexpr uintptr_t OptionCount     = 0x00C016FC;  // uint32_t

// --- GossipQuestEntry array layout ---

inline constexpr size_t    EntryStride     = 0x214;   // bytes per entry
inline constexpr size_t    MaxEntries      = 32;      // max quests in array

// --- GossipQuestEntry field offsets (from entry base) ---

inline constexpr uintptr_t QuestId         = 0x000;   // uint32_t
inline constexpr uintptr_t QuestLevel      = 0x004;   // int32_t
inline constexpr uintptr_t QuestFlags      = 0x008;   // uint32_t
inline constexpr uintptr_t AutoComplete    = 0x00C;   // uint32_t
inline constexpr uintptr_t QuestIcon       = 0x010;   // uint32_t
inline constexpr uintptr_t Title           = 0x014;   // char[0x200]

// questIcon values that indicate an ACTIVE quest (in player's quest log):
inline constexpr uint32_t  IconActive1     = 3;       // in-progress (gray ?)
inline constexpr uint32_t  IconActive2     = 4;       // completable (yellow ?)

// quest flag bits
inline constexpr uint32_t  FlagDaily       = 0x1000;
inline constexpr uint32_t  FlagRepeatable  = 0x0040;  // QUEST_FLAGS_REPEATABLE

// --- Function addresses ---

// CGQuestInfo_C::GetNumAvailGossipQuests() -> int
inline constexpr uintptr_t GetNumAvail     = 0x0058A5D0;

// CGQuestInfo_C::GetAvailableQuestInfoFromIndex(int idx0based) -> GossipQuestEntry*
inline constexpr uintptr_t GetAvailByIdx   = 0x0058A660;

// CGQuestInfo_C::GetNumActiveGossipQuests() -> int
inline constexpr uintptr_t GetNumActive    = 0x0058A6C0;

// CGQuestInfo_C::GetActiveQuestFromIndex(int idx0based) -> GossipQuestEntry*
inline constexpr uintptr_t GetActiveByIdx  = 0x0058A750;

// SetGossipObjectGUID(uint64_t* guid) — stores NPC GUID, clears old data
inline constexpr uintptr_t SetObjectGUID   = 0x0058A550;

// ClearGossipQuests() — zeros the entry array
inline constexpr uintptr_t ClearEntries    = 0x0058A7B0;

// SMSG_GOSSIP_MESSAGE packet handler — populates entry array from network data
inline constexpr uintptr_t PacketHandler   = 0x0058B1B0;

} // namespace offsets::gossip
