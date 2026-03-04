#pragma once
// =============================================================================
// Quest log memory layout (build 12340) — reverse-engineered via disasm_questlog.py
//
// The client maintains a flat array of quest log entries updated by the server.
// Each entry is 0x10 bytes:
//
//   struct QuestLogEntry {          // stride = 0x10
//       uint32_t questId;   // +0x00  questId for quest entries; zone id for headers
//       uint32_t unk04;     // +0x04  slot index used internally
//       uint32_t isHeader;  // +0x08  non-zero = zone header row (skip for quest ops)
//       uint32_t unk0C;     // +0x0C  isCollapsed? (for headers)
//   };
//
// Verified via inner_GetTitle (0x5E0000), inner_GetIsComplete (0x5DED30):
//   questId = *(uint32_t*)(EntryArrayBase + N*EntryStride + QuestId)
//   isHeader field non-zero → this is a zone-header row, not a real quest
//
// GetQuestLogTitle(i) return order (1-based Lua index):
//   [1] title  [2] level  [3] tag  [4] isComplete  [5] isHeader  [6] isCollapsed
// =============================================================================

namespace offsets::questlog {

inline constexpr uintptr_t TotalEntryCount  = 0x00C23AD0; // uint32_t — raw total (incl. headers)
inline constexpr uintptr_t DisplayedCount   = 0x00C23AE4; // uint32_t — select(1,GetNumQuestLogEntries())
inline constexpr uintptr_t SelectedQuestId  = 0x00C23AD8; // uint32_t — currently selected quest

inline constexpr uintptr_t EntryArrayBase   = 0x00C237B0; // first entry start
inline constexpr size_t    EntryStride      = 0x10;
inline constexpr size_t    MaxEntries       = 32;

// Offsets within each entry (relative to entry base)
inline constexpr uintptr_t QuestId          = 0x00; // uint32_t
inline constexpr uintptr_t IsHeader         = 0x08; // uint32_t, non-zero = zone header

// GetQuestLogTitle return value positions (1-based)
inline constexpr int TitleRet      = 1;
inline constexpr int LevelRet      = 2;
inline constexpr int TagRet        = 3;
inline constexpr int IsCompleteRet = 4; // 1=done, -1=failed, 0=not done
inline constexpr int IsHeaderRet   = 5;
inline constexpr int IsCollapsedRet= 6;

} // namespace offsets::questlog
