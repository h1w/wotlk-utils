#include "accept_quest.h"
#include "../../game/world.h"
#include "../../game/lua_bridge.h"
#include "../../game/quest.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdio>
#include <glog/logging.h>

namespace bot {

AcceptQuestTool::AcceptQuestTool(game::GUID npcGuid, int questId)
    : m_npcGuid(npcGuid), m_questId(questId)
{
}

void AcceptQuestTool::Start()
{
    if (!game::world::IsInGame()) {
        m_status = ToolStatus::Failed;
        return;
    }

    if (!m_approach.Init(m_npcGuid)) {
        LOG(WARNING) << "[AcceptQuestTool] NPC not found (questId=" << m_questId << ")";
        m_status = ToolStatus::Failed;
        return;
    }

    m_phase  = Phase::Approaching;
    m_status = ToolStatus::Running;
}

void AcceptQuestTool::Tick()
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
            m_phase = Phase::FindAndSelect;
        break;
    }

    case Phase::FindAndSelect: {
        bool greet = m_approach.UseGreeting();
        int idx = game::quest::FindGossipIndexByQuestId(m_questId, true, greet);
        if (idx == 0) {
            LOG(WARNING) << "[AcceptQuestTool] Quest " << m_questId
                         << " not found in gossip available list";
            m_status = ToolStatus::Failed;
            return;
        }

        if (greet)
            game::lua::Executef("SelectAvailableQuest(%d)", idx);
        else
            game::lua::Executef("SelectGossipAvailableQuest(%d)", idx);

        m_phase      = Phase::WaitDetail;
        m_phaseStart = now;
        break;
    }

    case Phase::WaitDetail: {
        if (now - m_phaseStart > kDetailTimeoutMs) {
            LOG(WARNING) << "[AcceptQuestTool] Timed out waiting for quest detail (questId="
                         << m_questId << ")";
            m_status = ToolStatus::Failed;
            return;
        }

        // GetTitleText() is non-empty when the quest detail frame is open.
        // GetQuestID() is not a registered Lua function in build 12340.
        std::string title = game::lua::GetValue("GetTitleText()");
        if (!title.empty() && title != "nil")
            m_phase = Phase::Accept;
        break;
    }

    case Phase::Accept: {
        // Record quest count before accepting so VerifyAccepted can detect the change.
        // GetQuestLogTitle in build 12340 does not return questID, so count-based
        // verification is the only reliable in-Lua approach.
        m_questCountBefore = game::lua::GetInt("select(2,GetNumQuestLogEntries())");
        game::lua::Execute("AcceptQuest()");
        m_phase      = Phase::VerifyAccepted;
        m_phaseStart = now;
        break;
    }

    case Phase::VerifyAccepted: {
        if (now - m_phaseStart > kVerifyTimeoutMs) {
            LOG(WARNING) << "[AcceptQuestTool] Quest " << m_questId
                         << " not found in log after accept (server rejected?)";
            m_status = ToolStatus::Failed;
            return;
        }

        int currentCount = game::lua::GetInt("select(2,GetNumQuestLogEntries())");
        if (currentCount > m_questCountBefore) {
            LOG(INFO) << "[AcceptQuestTool] Quest " << m_questId << " accepted successfully";
            m_status = ToolStatus::Completed;
        }
        break;
    }
    }
}

void AcceptQuestTool::Abort()
{
    if (m_status == ToolStatus::Running || m_status == ToolStatus::Pending) {
        m_approach.Abort();
        m_status = ToolStatus::Cancelled;
    }
}

std::string AcceptQuestTool::Describe() const
{
    char buf[128];
    const std::string& name = m_approach.GetNpcName();
    if (name.empty())
        snprintf(buf, sizeof(buf), "AcceptQuest id=%d", m_questId);
    else
        snprintf(buf, sizeof(buf), "AcceptQuest id=%d \"%s\"", m_questId, name.c_str());
    return buf;
}

} // namespace bot
