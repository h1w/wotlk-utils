#pragma once
// =============================================================================
// QueryQuestsTool — approach NPC, open gossip, read all quests with IDs.
//
// After Completed, call GetAvailableQuests() / GetActiveQuests() to retrieve
// the quest list. Quest IDs are read from memory (TASK_010 deliverable).
// =============================================================================

#include "../tool.h"
#include "quest_approach.h"
#include "../../game/types.h"
#include "../../game/quest.h"

#include <cstdint>
#include <vector>

namespace bot {

class QueryQuestsTool : public ITool {
public:
    explicit QueryQuestsTool(game::GUID npcGuid);

    ToolType    GetType() const override   { return ToolType::QueryQuests; }
    const char* GetName() const override   { return "QueryQuests"; }
    ToolStatus  GetStatus() const override { return m_status; }

    void Start() override;
    void Tick() override;
    void Abort() override;

    std::string Describe() const override;

    // Result accessors — valid after status == Completed
    const std::vector<game::quest::QuestInfo>& GetAvailableQuests() const { return m_available; }
    const std::vector<game::quest::QuestInfo>& GetActiveQuests()    const { return m_active; }

private:
    enum class Phase : uint8_t {
        Approaching,
        ReadQuests,
    };

    game::GUID  m_npcGuid;
    ToolStatus  m_status = ToolStatus::Pending;
    Phase       m_phase  = Phase::Approaching;

    QuestApproachHelper m_approach;

    std::vector<game::quest::QuestInfo> m_available;
    std::vector<game::quest::QuestInfo> m_active;
};

} // namespace bot
