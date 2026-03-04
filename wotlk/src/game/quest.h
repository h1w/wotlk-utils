#pragma once
// =============================================================================
// Quest utilities — gossip quest ID extraction, quest log helpers, and
// Lua-based quest data readers for quest interaction tools (TASK_011).
//
// The WoW 3.3.5a Lua API does NOT expose quest IDs from gossip windows.
// GetGossipQuestId() reads the internal CGQuestInfo array directly (TASK_010).
// =============================================================================

#include <vector>
#include <string>
#include <functional>

namespace game::quest {

// ---------------------------------------------------------------------------
// Gossip quest IDs (TASK_010 deliverable)
// ---------------------------------------------------------------------------

// Returns the quest ID for the gossip quest at the given index.
// gossipIndex: 1-based, matching the Lua GetGossipAvailableQuests() convention.
// isAvailable: true  = available quests (yellow ! — can be picked up)
//              false = active quests    (? — already in quest log)
// Returns 0 on failure: gossip not open, index out of range, or slot empty.
int GetGossipQuestId(int gossipIndex, bool isAvailable);

// Returns all gossip quest IDs for available or active quests.
std::vector<int> GetAllGossipQuestIds(bool isAvailable);

// Returns the count of available or active gossip quests.
int GetGossipQuestCount(bool isAvailable);

// ---------------------------------------------------------------------------
// Gossip quest metadata (raw memory read)
// ---------------------------------------------------------------------------

struct GossipQuestInfo {
    int      questId;
    int      questLevel;    // -1 for profession/special quests
    bool     isDaily;
    bool     isRepeatable;
    bool     autoComplete;
    std::string title;
};

// Returns metadata for a gossip quest. questId == 0 on failure.
GossipQuestInfo GetGossipQuestInfo(int gossipIndex, bool isAvailable);

// Returns metadata for all gossip quests.
std::vector<GossipQuestInfo> GetAllGossipQuestInfos(bool isAvailable);

// ---------------------------------------------------------------------------
// Combined quest data (Lua + memory) — for quest tools (TASK_011)
// ---------------------------------------------------------------------------

// Per-quest data combining Lua API title/level/flags with memory-read quest ID.
struct QuestInfo {
    int         questId;        // 0 if unavailable (QUEST_GREETING available quests)
    std::string title;
    int         level;
    bool        isLowLevel;
    bool        isDaily;
    bool        isRepeatable;   // set for available quests
    bool        isComplete;     // set for active quests
};

// Read available quests from gossip or QUEST_GREETING window.
// useGreeting: if true, uses GetNumAvailableQuests/GetAvailableTitle API
//              (for pure-quest NPCs that don't open a gossip frame).
// Quest IDs for greeting available quests will be 0 (no memory path available).
std::vector<QuestInfo> ReadGossipAvailableQuests(bool useGreeting = false);

// Read active (in-log) quests from gossip or QUEST_GREETING window.
// For greeting NPCs, quest IDs are cross-referenced from the quest log by title.
std::vector<QuestInfo> ReadGossipActiveQuests(bool useGreeting = false);

// ---------------------------------------------------------------------------
// Reward screen readers — call while QUEST_COMPLETE screen is open
// ---------------------------------------------------------------------------

// A single reward item (choice or fixed/guaranteed).
struct QuestReward {
    int         index;      // 1-based index in the reward list
    int         itemId;     // parsed from item link; 0 if parsing fails
    std::string name;
    int         count;
    int         quality;    // 0=Poor, 1=Common, 2=Uncommon, 3=Rare, 4=Epic
};

std::vector<QuestReward> ReadRewardChoices();   // selectable rewards
std::vector<QuestReward> ReadFixedRewards();    // guaranteed (non-choice) rewards
int ReadRewardMoney();                          // copper
int ReadRewardXP();

// ---------------------------------------------------------------------------
// Quest log helpers
// ---------------------------------------------------------------------------

// Returns true if the quest is in the player's quest log.
bool IsQuestInLog(int questId);

// Returns true if the quest is in the log AND marked complete (objectives done).
bool IsQuestCompleteInLog(int questId);

// ---------------------------------------------------------------------------
// Gossip search helpers
// ---------------------------------------------------------------------------

// Returns true if a gossip NPC window is currently open (GUID slot non-zero).
bool IsGossipWindowOpen();

// Returns 1-based gossip index for a quest with the given ID, or 0 if not found.
// For QUEST_GREETING active quests (useGreeting=true), cross-references quest log.
// For QUEST_GREETING available quests, returns 0 (IDs unavailable).
int FindGossipIndexByQuestId(int questId, bool isAvailable, bool useGreeting = false);

// ---------------------------------------------------------------------------
// Utility
// ---------------------------------------------------------------------------

// Parse item ID from a WoW item link ("item:12345:..."). Returns 0 on failure.
int ParseItemIdFromLink(const std::string& link);

} // namespace game::quest
