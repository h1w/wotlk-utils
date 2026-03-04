#include "query_quests.h"
#include "../../game/world.h"

#include <cstdio>
#include <glog/logging.h>

namespace bot {

QueryQuestsTool::QueryQuestsTool(game::GUID npcGuid)
    : m_npcGuid(npcGuid)
{
}

void QueryQuestsTool::Start()
{
    if (!game::world::IsInGame()) {
        m_status = ToolStatus::Failed;
        return;
    }

    if (!m_approach.Init(m_npcGuid)) {
        LOG(WARNING) << "[QueryQuestsTool] NPC not found";
        m_status = ToolStatus::Failed;
        return;
    }

    m_phase  = Phase::Approaching;
    m_status = ToolStatus::Running;
}

void QueryQuestsTool::Tick()
{
    if (m_status != ToolStatus::Running)
        return;

    switch (m_phase) {
    case Phase::Approaching: {
        auto s = m_approach.Tick();
        if (s == QuestApproachHelper::Status::Failed) {
            m_status = ToolStatus::Failed;
            return;
        }
        if (s == QuestApproachHelper::Status::GossipOpen)
            m_phase = Phase::ReadQuests;
        break;
    }

    case Phase::ReadQuests: {
        bool greet = m_approach.UseGreeting();
        m_available = game::quest::ReadGossipAvailableQuests(greet);
        m_active    = game::quest::ReadGossipActiveQuests(greet);

        LOG(INFO) << "[QueryQuestsTool] \"" << m_approach.GetNpcName()
                  << "\": " << m_available.size() << " available, "
                  << m_active.size() << " active";

        for (const auto& q : m_available)
            LOG(INFO) << "[QueryQuestsTool]   AVAIL id=" << q.questId
                      << " lv=" << q.level << " \"" << q.title << "\""
                      << (q.isDaily ? " [daily]" : "")
                      << (q.isRepeatable ? " [rep]" : "");

        for (const auto& q : m_active)
            LOG(INFO) << "[QueryQuestsTool]   ACTIVE id=" << q.questId
                      << " lv=" << q.level << " \"" << q.title << "\""
                      << (q.isComplete ? " [COMPLETE]" : "");

        m_status = ToolStatus::Completed;
        break;
    }
    }
}

void QueryQuestsTool::Abort()
{
    if (m_status == ToolStatus::Running || m_status == ToolStatus::Pending) {
        m_approach.Abort();
        m_status = ToolStatus::Cancelled;
    }
}

std::string QueryQuestsTool::Describe() const
{
    const std::string& name = m_approach.GetNpcName();
    if (name.empty())
        return "QueryQuests";
    char buf[128];
    snprintf(buf, sizeof(buf), "QueryQuests \"%s\"", name.c_str());
    return buf;
}

} // namespace bot
