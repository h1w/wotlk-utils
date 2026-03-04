#pragma once
// =============================================================================
// QueryRewardsTool — navigate to quest completion reward screen, read rewards,
// then close the dialog WITHOUT completing the quest (preview-only).
//
// Flow: Approaching → FindAndSelectActive → WaitProgress → CallComplete →
//       WaitRewardScreen → ReadRewards → CloseDialog
// =============================================================================

#include "../tool.h"
#include "quest_approach.h"
#include "../../game/types.h"
#include "../../game/quest.h"

#include <cstdint>
#include <vector>

namespace bot {

class QueryRewardsTool : public ITool {
public:
    QueryRewardsTool(game::GUID npcGuid, int questId);

    ToolType    GetType() const override   { return ToolType::QueryRewards; }
    const char* GetName() const override   { return "QueryRewards"; }
    ToolStatus  GetStatus() const override { return m_status; }

    void Start() override;
    void Tick() override;
    void Abort() override;

    std::string Describe() const override;

    // Result accessors — valid after status == Completed
    const std::vector<game::quest::QuestReward>& GetRewardChoices() const { return m_choices; }
    const std::vector<game::quest::QuestReward>& GetFixedRewards()  const { return m_fixed; }
    int GetRewardMoney() const { return m_money; }
    int GetRewardXP()    const { return m_xp; }

private:
    enum class Phase : uint8_t {
        Approaching,
        FindAndSelectActive,  // find quest by ID in active list, SelectGossipActiveQuest
        WaitProgress,         // wait for quest progress/complete frame
        CallComplete,         // call CompleteQuest() to advance to reward screen
        WaitRewardScreen,     // wait for reward screen (GetNumQuestChoices >= 0)
        ReadRewards,          // read reward data, single frame
        CloseDialog,          // call CloseQuest(), single frame
    };

    game::GUID  m_npcGuid;
    int         m_questId;
    ToolStatus  m_status        = ToolStatus::Pending;
    Phase       m_phase         = Phase::Approaching;
    bool        m_isAutoComplete = false;

    QuestApproachHelper m_approach;

    uint64_t    m_phaseStart = 0;

    std::vector<game::quest::QuestReward> m_choices;
    std::vector<game::quest::QuestReward> m_fixed;
    int         m_money = 0;
    int         m_xp    = 0;

    static constexpr uint32_t kProgressTimeoutMs = 3000;
    static constexpr uint32_t kRewardTimeoutMs   = 3000;
};

} // namespace bot
