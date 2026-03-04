#include "quest.h"
#include "mem.h"
#include "lua_bridge.h"
#include "../offsets/gossip.h"
#include "../offsets/questgiver.h"
#include "../offsets/questlog.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

// =============================================================================
// Implementation notes:
//
// The gossip quest data lives in a static flat array at 0x00BFC968, populated
// by the SMSG_GOSSIP_MESSAGE packet handler. Each slot is 0x214 bytes:
//
//   struct GossipQuestEntry {
//       uint32_t questId;       // +0x000  (0 = unused slot)
//       int32_t  questLevel;    // +0x004
//       uint32_t questFlags;    // +0x008
//       uint32_t autoComplete;  // +0x00C
//       uint32_t questIcon;     // +0x010  (3/4 = active, else = available)
//       char     title[0x200];  // +0x014
//   };
//
// Available vs active is determined solely by questIcon:
//   available:  questIcon != 3 && questIcon != 4
//   active:     questIcon == 3 || questIcon == 4
//
// The array has 32 slots; slots beyond the last quest have questId == 0.
// =============================================================================

namespace game::quest {

namespace {

using namespace offsets::gossip;

// Returns true if the questIcon value indicates an active (in-log) quest.
inline bool IsActiveIcon(uint32_t icon)
{
    return icon == IconActive1 || icon == IconActive2;
}

// Returns the absolute address of the Nth entry (0-based raw slot index).
inline uintptr_t EntryAddr(size_t rawSlot)
{
    return EntryArrayBase + rawSlot * EntryStride;
}

// Read one gossip entry field with SEH safety; returns 0 on failure.
inline uint32_t ReadField(uintptr_t entryBase, uintptr_t fieldOffset)
{
    return mem::ReadU32(entryBase + fieldOffset);
}

// Walk the array and return the address of the Nth entry (0-based) matching
// the wanted type (available or active). Returns 0 if not found.
uintptr_t FindEntry(int idx0based, bool isAvailable)
{
    int matched = 0;
    for (size_t slot = 0; slot < MaxEntries; ++slot) {
        uintptr_t base = EntryAddr(slot);
        uint32_t  id   = ReadField(base, QuestId);
        if (id == 0)
            break; // array is packed; first zero marks end

        uint32_t icon = ReadField(base, QuestIcon);
        bool     isActive = IsActiveIcon(icon);

        if (isAvailable == !isActive) {
            if (matched == idx0based)
                return base;
            ++matched;
        }
    }
    return 0;
}

} // namespace


int GetGossipQuestId(int gossipIndex, bool isAvailable)
{
    if (gossipIndex < 1)
        return 0;

    uintptr_t entry = FindEntry(gossipIndex - 1, isAvailable); // convert to 0-based
    if (entry == 0)
        return 0;

    return static_cast<int>(ReadField(entry, QuestId));
}


std::vector<int> GetAllGossipQuestIds(bool isAvailable)
{
    std::vector<int> ids;
    ids.reserve(8);

    for (size_t slot = 0; slot < MaxEntries; ++slot) {
        uintptr_t base = EntryAddr(slot);
        uint32_t  id   = ReadField(base, QuestId);
        if (id == 0)
            break;

        uint32_t icon    = ReadField(base, QuestIcon);
        bool     isActive = IsActiveIcon(icon);

        if (isAvailable == !isActive)
            ids.push_back(static_cast<int>(id));
    }
    return ids;
}


int GetGossipQuestCount(bool isAvailable)
{
    int count = 0;
    for (size_t slot = 0; slot < MaxEntries; ++slot) {
        uintptr_t base = EntryAddr(slot);
        uint32_t  id   = ReadField(base, QuestId);
        if (id == 0)
            break;

        uint32_t icon    = ReadField(base, QuestIcon);
        bool     isActive = IsActiveIcon(icon);

        if (isAvailable == !isActive)
            ++count;
    }
    return count;
}


GossipQuestInfo GetGossipQuestInfo(int gossipIndex, bool isAvailable)
{
    GossipQuestInfo info{};
    if (gossipIndex < 1)
        return info;

    uintptr_t entry = FindEntry(gossipIndex - 1, isAvailable);
    if (entry == 0)
        return info;

    info.questId      = static_cast<int>(ReadField(entry, QuestId));
    info.questLevel   = static_cast<int>(ReadField(entry, QuestLevel));
    info.isDaily      = (ReadField(entry, QuestFlags) & FlagDaily) != 0;
    info.isRepeatable = (ReadField(entry, QuestFlags) & FlagRepeatable) != 0;
    info.autoComplete = ReadField(entry, AutoComplete) != 0;
    info.title        = mem::ReadCString(entry + Title, 0x200);
    return info;
}


std::vector<GossipQuestInfo> GetAllGossipQuestInfos(bool isAvailable)
{
    std::vector<GossipQuestInfo> infos;
    infos.reserve(8);

    for (size_t slot = 0; slot < MaxEntries; ++slot) {
        uintptr_t base = EntryAddr(slot);
        uint32_t  id   = ReadField(base, QuestId);
        if (id == 0)
            break;

        uint32_t icon    = ReadField(base, QuestIcon);
        bool     isActive = IsActiveIcon(icon);

        if (isAvailable != !isActive)
            continue;

        GossipQuestInfo info;
        info.questId      = static_cast<int>(id);
        info.questLevel   = static_cast<int>(ReadField(base, QuestLevel));
        info.isDaily      = (ReadField(base, QuestFlags) & FlagDaily) != 0;
        info.isRepeatable = (ReadField(base, QuestFlags) & FlagRepeatable) != 0;
        info.autoComplete = ReadField(base, AutoComplete) != 0;
        info.title        = mem::ReadCString(base + Title, 0x200);
        infos.push_back(std::move(info));
    }
    return infos;
}


// =============================================================================
// Combined quest data helpers (Lua + memory) — TASK_011
// =============================================================================

// ---------------------------------------------------------------------------
// ReadGossipAvailableQuests
// ---------------------------------------------------------------------------
std::vector<QuestInfo> ReadGossipAvailableQuests(bool useGreeting)
{
    std::vector<QuestInfo> result;

    if (!useGreeting) {
        int count = lua::GetInt("GetNumGossipAvailableQuests()");
        if (count <= 0)
            return result;

        // Store flat list into temp table: title(1), level(2), isLowLevel(3), isDaily(4), isRepeatable(5) per quest
        lua::Execute("_wqt={GetGossipAvailableQuests()}");
        result.reserve(count);

        for (int i = 0; i < count; ++i) {
            QuestInfo q{};
            q.questId      = GetGossipQuestId(i + 1, true);

            char buf[64];
            snprintf(buf, sizeof(buf), "_wqt[%d]", i * 5 + 1);  q.title       = lua::GetValue(buf);
            snprintf(buf, sizeof(buf), "_wqt[%d]", i * 5 + 2);  q.level       = lua::GetInt(buf);
            snprintf(buf, sizeof(buf), "_wqt[%d]", i * 5 + 3);  q.isLowLevel  = lua::GetBool(buf);
            snprintf(buf, sizeof(buf), "_wqt[%d]", i * 5 + 4);  q.isDaily     = lua::GetBool(buf);
            snprintf(buf, sizeof(buf), "_wqt[%d]", i * 5 + 5);  q.isRepeatable= lua::GetBool(buf);
            result.push_back(std::move(q));
        }
    } else {
        // QUEST_GREETING: questId from memory, title from GetAvailableTitle(i) (returns 1 value)
        int count = lua::GetInt("GetNumAvailableQuests()");
        if (count <= 0)
            return result;
        result.reserve(count);

        for (int i = 1; i <= count; ++i) {
            QuestInfo q{};
            // Read questId directly from the questgiver available array (offset +0x000)
            uintptr_t entry = offsets::questgiver::AvailArrayBase
                            + (i - 1) * offsets::questgiver::Stride;
            q.questId = static_cast<int>(mem::ReadU32(entry + offsets::questgiver::QuestId));

            // GetAvailableTitle(i) returns only the title string (1 return value)
            char buf[64];
            snprintf(buf, sizeof(buf), "GetAvailableTitle(%d)", i);
            q.title = lua::GetValue(buf);
            result.push_back(std::move(q));
        }
    }

    return result;
}


// ---------------------------------------------------------------------------
// ReadGossipActiveQuests
// ---------------------------------------------------------------------------
std::vector<QuestInfo> ReadGossipActiveQuests(bool useGreeting)
{
    std::vector<QuestInfo> result;

    if (!useGreeting) {
        int count = lua::GetInt("GetNumGossipActiveQuests()");
        if (count <= 0)
            return result;

        // Flat list: title(1), level(2), isLowLevel(3), isComplete(4) per quest
        lua::Execute("_wqt={GetGossipActiveQuests()}");
        result.reserve(count);

        for (int i = 0; i < count; ++i) {
            QuestInfo q{};
            q.questId    = GetGossipQuestId(i + 1, false);

            char buf[64];
            snprintf(buf, sizeof(buf), "_wqt[%d]", i * 4 + 1);  q.title      = lua::GetValue(buf);
            snprintf(buf, sizeof(buf), "_wqt[%d]", i * 4 + 2);  q.level      = lua::GetInt(buf);
            snprintf(buf, sizeof(buf), "_wqt[%d]", i * 4 + 3);  q.isLowLevel = lua::GetBool(buf);
            snprintf(buf, sizeof(buf), "_wqt[%d]", i * 4 + 4);  q.isComplete = lua::GetBool(buf);
            result.push_back(std::move(q));
        }
    } else {
        // QUEST_GREETING: questId from memory, title/isComplete from GetActiveTitle(i)
        int count = lua::GetInt("GetNumActiveQuests()");
        if (count <= 0)
            return result;
        result.reserve(count);

        for (int i = 1; i <= count; ++i) {
            QuestInfo q{};
            // Read questId directly from the questgiver active array (offset +0x000)
            uintptr_t entry = offsets::questgiver::ActiveArrayBase
                            + (i - 1) * offsets::questgiver::Stride;
            q.questId = static_cast<int>(mem::ReadU32(entry + offsets::questgiver::QuestId));

            // GetActiveTitle(i) returns: title[1], isComplete[2]
            char buf[64];
            snprintf(buf, sizeof(buf), "_wqt={GetActiveTitle(%d)}", i);
            lua::Execute(buf);

            q.title      = lua::GetValue("_wqt[1]");
            q.isComplete = lua::GetBool("_wqt[2]");
            result.push_back(std::move(q));
        }
    }

    return result;
}


// ---------------------------------------------------------------------------
// Reward screen readers
// ---------------------------------------------------------------------------
static QuestReward ReadRewardItem(const char* rewardType, int idx)
{
    QuestReward r{};
    r.index = idx;

    char buf[128];
    snprintf(buf, sizeof(buf), "_wqt={GetQuestItemInfo(\"%s\",%d)}", rewardType, idx);
    lua::Execute(buf);

    r.name    = lua::GetValue("_wqt[1]");
    // [2]=texture, [3]=count, [4]=quality, [5]=isUsable
    r.count   = lua::GetInt("_wqt[3]");
    r.quality = lua::GetInt("_wqt[4]");

    snprintf(buf, sizeof(buf), "GetQuestItemLink(\"%s\",%d)", rewardType, idx);
    r.itemId = ParseItemIdFromLink(lua::GetValue(buf));
    return r;
}

std::vector<QuestReward> ReadRewardChoices()
{
    std::vector<QuestReward> result;
    int count = lua::GetInt("GetNumQuestChoices()");
    result.reserve(count);
    for (int i = 1; i <= count; ++i)
        result.push_back(ReadRewardItem("choice", i));
    return result;
}

std::vector<QuestReward> ReadFixedRewards()
{
    std::vector<QuestReward> result;
    int count = lua::GetInt("GetNumQuestRewards()");
    result.reserve(count);
    for (int i = 1; i <= count; ++i)
        result.push_back(ReadRewardItem("reward", i));
    return result;
}

int ReadRewardMoney() { return lua::GetInt("GetRewardMoney()"); }
int ReadRewardXP()    { return lua::GetInt("GetRewardXP()"); }


// ---------------------------------------------------------------------------
// Quest log helpers
// ---------------------------------------------------------------------------
bool IsQuestInLog(int questId)
{
    // Direct memory scan of the quest log array (RE'd via disasm_questlog.py).
    // GetQuestLogIndexByID is not registered in build 12340.
    // questId stored at entry+0x00; isHeader at entry+0x08 (non-zero = skip).
    namespace ql = offsets::questlog;
    uint32_t count = mem::ReadU32(ql::TotalEntryCount);
    for (uint32_t i = 0; i < count && i < ql::MaxEntries; ++i) {
        uintptr_t entry = ql::EntryArrayBase + i * ql::EntryStride;
        if (mem::ReadU32(entry + ql::IsHeader) != 0)
            continue; // zone header row
        if (static_cast<int>(mem::ReadU32(entry + ql::QuestId)) == questId)
            return true;
    }
    return false;
}

bool IsQuestCompleteInLog(int questId)
{
    // Find quest's 1-based Lua index via memory, then read isComplete from
    // GetQuestLogTitle(i) return position 4 (RE'd: pos 7 = frequency, not isComplete).
    namespace ql = offsets::questlog;
    uint32_t count = mem::ReadU32(ql::TotalEntryCount);
    for (uint32_t i = 0; i < count && i < ql::MaxEntries; ++i) {
        uintptr_t entry = ql::EntryArrayBase + i * ql::EntryStride;
        if (mem::ReadU32(entry + ql::IsHeader) != 0)
            continue;
        if (static_cast<int>(mem::ReadU32(entry + ql::QuestId)) != questId)
            continue;
        // Found at 0-based index i → 1-based Lua index i+1
        char buf[64];
        snprintf(buf, sizeof(buf), "select(%d,GetQuestLogTitle(%u))", ql::IsCompleteRet, i + 1);
        return lua::GetInt(buf) == 1; // 1=done, -1=failed, 0=not done
    }
    return false;
}


// ---------------------------------------------------------------------------
// Gossip search helpers
// ---------------------------------------------------------------------------
bool IsGossipWindowOpen()
{
    return mem::ReadU64(offsets::gossip::ObjectGUID) != 0;
}

int FindGossipIndexByQuestId(int questId, bool isAvailable, bool useGreeting)
{
    if (!useGreeting) {
        int count = GetGossipQuestCount(isAvailable);
        for (int i = 1; i <= count; ++i) {
            if (GetGossipQuestId(i, isAvailable) == questId)
                return i;
        }
        return 0;
    }

    // QUEST_GREETING: scan questgiver array directly for matching questId
    uintptr_t arrayBase = isAvailable
        ? offsets::questgiver::AvailArrayBase
        : offsets::questgiver::ActiveArrayBase;
    uint32_t count = mem::ReadU32(isAvailable
        ? offsets::questgiver::AvailCount
        : offsets::questgiver::ActiveCount);

    for (uint32_t i = 0; i < count && i < offsets::questgiver::MaxEntries; ++i) {
        uintptr_t entry = arrayBase + i * offsets::questgiver::Stride;
        if (static_cast<int>(mem::ReadU32(entry + offsets::questgiver::QuestId)) == questId)
            return static_cast<int>(i + 1);  // 1-based Lua index
    }
    return 0;
}


// ---------------------------------------------------------------------------
// Utility
// ---------------------------------------------------------------------------
int ParseItemIdFromLink(const std::string& link)
{
    auto pos = link.find("item:");
    if (pos == std::string::npos)
        return 0;
    return std::atoi(link.c_str() + pos + 5);  // skip "item:", stop at ':'
}

} // namespace game::quest
