#include "complete_quest.h"
#include "../../game/world.h"
#include "../../game/mem.h"
#include "../../game/lua_bridge.h"
#include "../../game/quest.h"
#include "../../offsets/questgiver.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdio>
#include <glog/logging.h>

namespace bot {

CompleteQuestTool::CompleteQuestTool(game::GUID npcGuid, int questId, RewardCallback callback)
    : m_npcGuid(npcGuid), m_questId(questId), m_callback(std::move(callback))
{
}

void CompleteQuestTool::Start()
{
    if (!game::world::IsInGame()) {
        m_status = ToolStatus::Failed;
        return;
    }

    if (!m_approach.Init(m_npcGuid)) {
        LOG(WARNING) << "[CompleteQuestTool] NPC not found (questId=" << m_questId << ")";
        m_status = ToolStatus::Failed;
        return;
    }

    m_phase  = Phase::Approaching;
    m_status = ToolStatus::Running;
}

void CompleteQuestTool::Tick()
{
    if (m_status != ToolStatus::Running)
        return;

    uint64_t now = GetTickCount64();

    switch (m_phase) {
    case Phase::Approaching: {
        auto s = m_approach.Tick();
        if (s == QuestApproachHelper::Status::Failed) {
            m_status = ToolStatus::Failed;
            return;
        }
        if (s == QuestApproachHelper::Status::GossipOpen)
            m_phase = Phase::FindAndSelectActive;
        break;
    }

    case Phase::FindAndSelectActive: {
        // Server may auto-select and jump straight to offer-reward frame — skip quest list navigation.
        if (m_approach.IsQuestFrameOpen()) {
            m_isAutoComplete = false;
            m_phase      = Phase::WaitProgress;
            m_phaseStart = now;
            break;
        }

        bool greet = m_approach.UseGreeting();
        int idx = game::quest::FindGossipIndexByQuestId(m_questId, false, greet);
        if (idx != 0) {
            // Found in active list — regular turn-in flow.
            m_isAutoComplete = false;
            if (greet)
                game::lua::Executef("SelectActiveQuest(%d)", idx);
            else
                game::lua::Executef("SelectGossipActiveQuest(%d)", idx);
            m_phase      = Phase::WaitProgress;
            m_phaseStart = now;
            break;
        }

        // Not in active list — check available list for auto-complete quests.
        idx = game::quest::FindGossipIndexByQuestId(m_questId, true, greet);
        if (idx != 0) {
            bool ac = !greet
                ? game::quest::GetGossipQuestInfo(idx, true).autoComplete
                : game::mem::ReadU32(offsets::questgiver::AvailArrayBase
                      + (idx - 1) * offsets::questgiver::Stride
                      + offsets::questgiver::AutoComplete) != 0;
            if (ac) {
                m_isAutoComplete = true;
                LOG(INFO) << "[CompleteQuestTool] Quest " << m_questId << " is auto-complete";
                if (greet)
                    game::lua::Executef("SelectAvailableQuest(%d)", idx);
                else
                    game::lua::Executef("SelectGossipAvailableQuest(%d)", idx);
                m_phase      = Phase::WaitProgress;
                m_phaseStart = now;
                break;
            }
        }

        LOG(WARNING) << "[CompleteQuestTool] Quest " << m_questId
                     << " not found in gossip (active or auto-complete)";
        m_status = ToolStatus::Failed;
        return;
    }

    case Phase::WaitProgress: {
        if (now - m_phaseStart > kProgressTimeoutMs) {
            LOG(WARNING) << "[CompleteQuestTool] Timed out waiting for progress frame (questId="
                         << m_questId << ")";
            m_status = ToolStatus::Failed;
            return;
        }

        std::string titleText = game::lua::GetValue("GetTitleText()");
        if (!titleText.empty() && titleText != "nil")
            m_phase = Phase::CallComplete;
        break;
    }

    case Phase::CallComplete: {
        if (m_isAutoComplete) {
            // Auto-complete: single call grants rewards + removes quest from log.
            // No reward screen shown — jump straight to verify.
            game::lua::Execute("CompleteAutoQuest()");
            m_phase      = Phase::VerifyCompleted;
            m_phaseStart = now;
        } else {
            game::lua::Execute("CompleteQuest()");
            m_phase      = Phase::WaitRewardScreen;
            m_phaseStart = now;
        }
        break;
    }

    case Phase::WaitRewardScreen: {
        if (now - m_phaseStart > kRewardTimeoutMs) {
            LOG(WARNING) << "[CompleteQuestTool] Timed out waiting for reward screen (questId="
                         << m_questId << ")";
            game::lua::Execute("CloseQuest()");
            m_status = ToolStatus::Failed;
            return;
        }

        std::string val = game::lua::GetValue("GetNumQuestChoices()");
        if (val != "nil" && !val.empty())
            m_phase = Phase::ResolveReward;
        break;
    }

    case Phase::ResolveReward: {
        auto choices = game::quest::ReadRewardChoices();
        int numChoices = static_cast<int>(choices.size());

        if (numChoices == 0) {
            // No selectable rewards — use no-choice finalizer
            m_choiceIndex = 0;
            m_phase = Phase::FinalizeReward;
            break;
        }

        if (!m_callback) {
            LOG(WARNING) << "[CompleteQuestTool] Quest " << m_questId
                         << " has " << numChoices << " choices but no callback provided";
            game::lua::Execute("CloseQuest()");
            m_status = ToolStatus::Failed;
            return;
        }

        m_choiceIndex = m_callback(choices);

        if (m_choiceIndex < 1 || m_choiceIndex > numChoices) {
            LOG(WARNING) << "[CompleteQuestTool] Callback returned invalid choice index "
                         << m_choiceIndex << " (valid: 1-" << numChoices << ")";
            game::lua::Execute("CloseQuest()");
            m_status = ToolStatus::Failed;
            return;
        }

        m_phase = Phase::FinalizeReward;
        break;
    }

    case Phase::FinalizeReward: {
        // IRREVERSIBLE: grants rewards and removes quest from log
        game::lua::Executef("GetQuestReward(%d)", m_choiceIndex);
        m_phase      = Phase::VerifyCompleted;
        m_phaseStart = now;
        break;
    }

    case Phase::VerifyCompleted: {
        if (now - m_phaseStart > kVerifyTimeoutMs) {
            LOG(WARNING) << "[CompleteQuestTool] Quest " << m_questId
                         << " still in log after GetQuestReward (server rejected?)";
            m_status = ToolStatus::Failed;
            return;
        }

        if (!game::quest::IsQuestInLog(m_questId)) {
            LOG(INFO) << "[CompleteQuestTool] Quest " << m_questId << " completed successfully";
            m_status = ToolStatus::Completed;
        }
        break;
    }
    }
}

void CompleteQuestTool::Abort()
{
    if (m_status == ToolStatus::Running || m_status == ToolStatus::Pending) {
        m_approach.Abort();
        // Only close if we haven't yet finalized (FinalizeReward is irreversible)
        if (m_phase != Phase::FinalizeReward && m_phase != Phase::VerifyCompleted)
            game::lua::Execute("CloseQuest()");
        m_status = ToolStatus::Cancelled;
    }
}

std::string CompleteQuestTool::Describe() const
{
    char buf[128];
    const std::string& name = m_approach.GetNpcName();
    if (name.empty())
        snprintf(buf, sizeof(buf), "CompleteQuest id=%d", m_questId);
    else
        snprintf(buf, sizeof(buf), "CompleteQuest id=%d \"%s\"", m_questId, name.c_str());
    return buf;
}

} // namespace bot
