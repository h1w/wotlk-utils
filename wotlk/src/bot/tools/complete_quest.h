#pragma once
// =============================================================================
// CompleteQuestTool — approach NPC, navigate to reward screen, select reward
// via callback, finalize quest.
//
// Flow: Approaching → FindAndSelectActive → WaitProgress → CallComplete →
//       WaitRewardScreen → ResolveReward → FinalizeReward → VerifyCompleted
//
// RewardCallback receives the choice reward list and returns 1-based choice
// index (or 0 for no selection when choices exist → tool fails).
// If callback is nullptr and choices > 0, the tool fails.
// If choices == 0, GetQuestReward(0) is called (no-choice finalizer).
// =============================================================================

#include "../tool.h"
#include "quest_approach.h"
#include "../../game/types.h"
#include "../../game/quest.h"

#include <cstdint>
#include <functional>
#include <vector>

namespace bot {

class CompleteQuestTool : public ITool {
public:
    using RewardCallback = std::function<int(const std::vector<game::quest::QuestReward>& choices)>;

    // callback: receives reward choices, returns 1-based selected index.
    //           Pass nullptr if quest is known to have no choices.
    CompleteQuestTool(game::GUID npcGuid, int questId, RewardCallback callback = nullptr);

    ToolType    GetType() const override   { return ToolType::CompleteQuest; }
    const char* GetName() const override   { return "CompleteQuest"; }
    ToolStatus  GetStatus() const override { return m_status; }

    void Start() override;
    void Tick() override;
    void Abort() override;

    std::string Describe() const override;

private:
    enum class Phase : uint8_t {
        Approaching,
        FindAndSelectActive,
        WaitProgress,
        CallComplete,
        WaitRewardScreen,
        ResolveReward,    // invoke callback, determine choice index
        FinalizeReward,   // call GetQuestReward(choiceIndex) — IRREVERSIBLE
        VerifyCompleted,  // wait for quest to leave log
    };

    game::GUID     m_npcGuid;
    int            m_questId;
    RewardCallback m_callback;
    ToolStatus     m_status       = ToolStatus::Pending;
    Phase          m_phase        = Phase::Approaching;
    bool           m_isAutoComplete = false;

    QuestApproachHelper m_approach;

    uint64_t    m_phaseStart  = 0;
    int         m_choiceIndex = 0;  // resolved choice (0 = no choice)

    static constexpr uint32_t kProgressTimeoutMs = 3000;
    static constexpr uint32_t kRewardTimeoutMs   = 3000;
    static constexpr uint32_t kVerifyTimeoutMs   = 3000;
};

} // namespace bot
