#pragma once
// =============================================================================
// AcceptQuestTool — approach NPC, find quest by ID in gossip, accept it.
//
// Flow: Approaching → WaitGossip → FindAndSelect → WaitDetail → Accept → VerifyAccepted
// =============================================================================

#include "../tool.h"
#include "quest_approach.h"
#include "../../game/types.h"

#include <cstdint>

namespace bot {

class AcceptQuestTool : public ITool {
public:
    AcceptQuestTool(game::GUID npcGuid, int questId);

    ToolType    GetType() const override   { return ToolType::AcceptQuest; }
    const char* GetName() const override   { return "AcceptQuest"; }
    ToolStatus  GetStatus() const override { return m_status; }

    void Start() override;
    void Tick() override;
    void Abort() override;

    std::string Describe() const override;

private:
    enum class Phase : uint8_t {
        Approaching,
        FindAndSelect,   // search gossip list by quest ID, call SelectGossipAvailableQuest
        WaitDetail,      // wait for QUEST_DETAIL frame to show with matching quest ID
        Accept,          // call AcceptQuest()
        VerifyAccepted,  // wait for quest to appear in log
    };

    game::GUID  m_npcGuid;
    int         m_questId;
    ToolStatus  m_status = ToolStatus::Pending;
    Phase       m_phase  = Phase::Approaching;

    QuestApproachHelper m_approach;

    uint64_t    m_phaseStart = 0;
    int         m_questCountBefore = 0;  // quest count before AcceptQuest() call

    static constexpr uint32_t kDetailTimeoutMs  = 3000;
    static constexpr uint32_t kVerifyTimeoutMs  = 5000;
};

} // namespace bot
