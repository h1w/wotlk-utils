#include "query_rewards.h"
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

QueryRewardsTool::QueryRewardsTool(game::GUID npcGuid, int questId)
    : m_npcGuid(npcGuid), m_questId(questId)
{
}

void QueryRewardsTool::Start()
{
    if (!game::world::IsInGame()) {
        m_status = ToolStatus::Failed;
        return;
    }

    if (!m_approach.Init(m_npcGuid)) {
        LOG(WARNING) << "[QueryRewardsTool] NPC not found (questId=" << m_questId << ")";
        m_status = ToolStatus::Failed;
        return;
    }

    m_phase  = Phase::Approaching;
    m_status = ToolStatus::Running;
}

void QueryRewardsTool::Tick()
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
                if (greet)
                    game::lua::Executef("SelectAvailableQuest(%d)", idx);
                else
                    game::lua::Executef("SelectGossipAvailableQuest(%d)", idx);
                m_phase      = Phase::WaitProgress;
                m_phaseStart = now;
                break;
            }
        }

        LOG(WARNING) << "[QueryRewardsTool] Quest " << m_questId
                     << " not found in gossip (active or auto-complete)";
        m_status = ToolStatus::Failed;
        return;
    }

    case Phase::WaitProgress: {
        if (now - m_phaseStart > kProgressTimeoutMs) {
            LOG(WARNING) << "[QueryRewardsTool] Timed out waiting for progress frame (questId="
                         << m_questId << ")";
            m_status = ToolStatus::Failed;
            return;
        }

        // IsQuestCompleteInLog tells us if objectives are done, but we need the frame to be open.
        // Poll GetNumQuestChoices() — returns nil/0 while progress frame is open (before CompleteQuest).
        // When the quest is completable, we can call CompleteQuest() immediately.
        // Detect the quest progress frame by checking if GetTitleText() returns non-nil.
        std::string titleText = game::lua::GetValue("GetTitleText()");
        if (!titleText.empty() && titleText != "nil")
            m_phase = Phase::CallComplete;
        break;
    }

    case Phase::CallComplete: {
        if (m_isAutoComplete) {
            // Auto-complete quests grant rewards silently — no reward screen.
            game::lua::Execute("CompleteAutoQuest()");
            LOG(INFO) << "[QueryRewardsTool] Quest " << m_questId
                      << " is auto-complete, rewards granted silently";
            m_status = ToolStatus::Completed;
        } else {
            game::lua::Execute("CompleteQuest()");
            m_phase      = Phase::WaitRewardScreen;
            m_phaseStart = now;
        }
        break;
    }

    case Phase::WaitRewardScreen: {
        if (now - m_phaseStart > kRewardTimeoutMs) {
            LOG(WARNING) << "[QueryRewardsTool] Timed out waiting for reward screen (questId="
                         << m_questId << ")";
            // Try to close whatever dialog is open
            game::lua::Execute("CloseQuest()");
            m_status = ToolStatus::Failed;
            return;
        }

        // GetNumQuestChoices() returns nil when reward screen is not open,
        // and >= 0 (including 0 for no-choice quests) when it is.
        std::string val = game::lua::GetValue("GetNumQuestChoices()");
        if (val != "nil" && !val.empty())
            m_phase = Phase::ReadRewards;
        break;
    }

    case Phase::ReadRewards: {
        m_choices = game::quest::ReadRewardChoices();
        m_fixed   = game::quest::ReadFixedRewards();
        m_money   = game::quest::ReadRewardMoney();
        m_xp      = game::quest::ReadRewardXP();

        LOG(INFO) << "[QueryRewardsTool] Quest " << m_questId << " rewards: "
                  << m_choices.size() << " choices, " << m_fixed.size() << " fixed, "
                  << m_money << "c, " << m_xp << "xp";

        for (const auto& r : m_choices)
            LOG(INFO) << "[QueryRewardsTool]   CHOICE[" << r.index << "] itemId="
                      << r.itemId << " \"" << r.name << "\" x" << r.count
                      << " q" << r.quality;
        for (const auto& r : m_fixed)
            LOG(INFO) << "[QueryRewardsTool]   FIXED[" << r.index << "] itemId="
                      << r.itemId << " \"" << r.name << "\" x" << r.count;

        m_phase = Phase::CloseDialog;
        break;
    }

    case Phase::CloseDialog: {
        game::lua::Execute("CloseQuest()");
        m_status = ToolStatus::Completed;
        break;
    }
    }
}

void QueryRewardsTool::Abort()
{
    if (m_status == ToolStatus::Running || m_status == ToolStatus::Pending) {
        m_approach.Abort();
        game::lua::Execute("CloseQuest()");
        m_status = ToolStatus::Cancelled;
    }
}

std::string QueryRewardsTool::Describe() const
{
    char buf[128];
    const std::string& name = m_approach.GetNpcName();
    if (name.empty())
        snprintf(buf, sizeof(buf), "QueryRewards id=%d", m_questId);
    else
        snprintf(buf, sizeof(buf), "QueryRewards id=%d \"%s\"", m_questId, name.c_str());
    return buf;
}

} // namespace bot
