#include "quest_approach.h"
#include "../../game/game.h"
#include "../../game/mem.h"
#include "../../game/movement.h"
#include "../../game/world.h"
#include "../../game/object_manager.h"
#include "../../game/lua_bridge.h"
#include "../../game/quest.h"
#include "../../offsets/gossip.h"
#include "../../offsets/questgiver.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <glog/logging.h>

namespace bot {

bool QuestApproachHelper::Init(game::GUID npcGuid)
{
    m_npcGuid = npcGuid;
    m_useNav = false;
    m_interactionIssued = false;
    m_useGreeting = false;
    m_alreadyOpen = false;
    m_firstPollLogged = false;
    m_questFrameOpen = false;
    m_phase = Phase::Approaching;

    uintptr_t ptr = game::objmgr::GetObjectPtr(m_npcGuid);
    if (ptr == 0)
        return false;

    game::WowObject obj(ptr);
    m_npcName = obj.GetName();
    m_npcPos  = obj.GetPosition();

    game::SelectTarget(m_npcGuid);

    m_startTick = GetTickCount64();

    // If the NPC's quest window is already open, skip movement and interaction.
    // Gossip window (GOSSIP_SHOW): ObjectGUID matches this NPC.
    if (game::mem::ReadU64(offsets::gossip::ObjectGUID) == npcGuid) {
        m_useGreeting = false;
        m_alreadyOpen = true;
        LOG(INFO) << "[QuestApproach] Gossip window already open for \"" << m_npcName << "\"";
        return true;
    }
    // QUEST_GREETING: questgiver NpcGUID matches AND live counts > 0.
    if (game::mem::ReadU64(offsets::questgiver::NpcGUID) == npcGuid) {
        int greetAvail  = game::lua::GetInt("GetNumAvailableQuests()");
        int greetActive = game::lua::GetInt("GetNumActiveQuests()");
        if (greetAvail + greetActive > 0) {
            m_useGreeting = true;
            m_alreadyOpen = true;
            LOG(INFO) << "[QuestApproach] QUEST_GREETING already open for \"" << m_npcName << "\"";
            return true;
        }
    }

    auto player = game::GetLocalPlayer();
    float dist = player ? player->GetPosition().DistanceTo(m_npcPos) : 0.0f;

    if (dist > kNavSwitchRange && m_nav.StartNavTo(m_npcPos)) {
        m_useNav = true;
        LOG(INFO) << "[QuestApproach] Using navmesh to approach \"" << m_npcName << "\" (" << dist << " yards)";
    } else {
        game::movement::ClickToMoveInteract(m_npcGuid, m_npcPos);
    }
    return true;
}

QuestApproachHelper::Status QuestApproachHelper::Tick()
{
    if (m_alreadyOpen)
        return Status::GossipOpen;

    uint64_t now = GetTickCount64();

    if (m_phase == Phase::Approaching) {
        if (now - m_startTick > kApproachTimeoutMs) {
            if (m_useNav) m_nav.Stop();
            LOG(WARNING) << "[QuestApproach] Approach timeout for \"" << m_npcName << "\"";
            return Status::Failed;
        }

        // Check if NPC still exists
        uintptr_t ptr = game::objmgr::GetObjectPtr(m_npcGuid);
        if (ptr == 0) {
            if (m_useNav) m_nav.Stop();
            LOG(WARNING) << "[QuestApproach] NPC \"" << m_npcName << "\" disappeared";
            return Status::Failed;
        }

        game::WowObject obj(ptr);
        m_npcPos = obj.GetPosition();

        auto player = game::GetLocalPlayer();
        if (!player)
            return Status::Failed;

        float dist = player->GetPosition().DistanceTo(m_npcPos);

        // Nav mode
        if (m_useNav) {
            if (dist <= kNavSwitchRange) {
                m_nav.Stop();
                m_useNav = false;
                game::SelectTarget(m_npcGuid);
                game::movement::ClickToMoveInteract(m_npcGuid, m_npcPos);
            } else {
                auto navStatus = m_nav.Tick();
                if (navStatus == NavHelper::Status::Arrived || navStatus == NavHelper::Status::Failed) {
                    m_useNav = false;
                    game::SelectTarget(m_npcGuid);
                    game::movement::ClickToMoveInteract(m_npcGuid, m_npcPos);
                }
            }
            return Status::Ongoing;
        }

        // Direct CTM mode — issue InteractUnit once we're in range
        if (dist <= kInteractRange) {
            if (!m_interactionIssued) {
                game::SelectTarget(m_npcGuid);
                game::lua::Execute("InteractUnit(\"target\")");
                m_interactionIssued = true;
                m_phase = Phase::WaitGossip;
                m_gossipWaitStart = now;
                LOG(INFO) << "[QuestApproach] Interaction issued, waiting for gossip/greeting";
            }
        }
        return Status::Ongoing;
    }

    // Phase::WaitGossip
    if (now - m_gossipWaitStart > kGossipTimeoutMs) {
        LOG(WARNING) << "[QuestApproach] Gossip wait timeout for \"" << m_npcName << "\""
                     << " (gossipAvail=" << game::lua::GetInt("GetNumGossipAvailableQuests()")
                     << " gossipActive=" << game::lua::GetInt("GetNumGossipActiveQuests()")
                     << " gossipWindowOpen=" << game::quest::IsGossipWindowOpen()
                     << " gossipObjGUID=0x" << std::hex << game::mem::ReadU64(offsets::gossip::ObjectGUID) << std::dec
                     << " greetAvail=" << game::lua::GetInt("GetNumAvailableQuests()")
                     << " greetActive=" << game::lua::GetInt("GetNumActiveQuests()") << ")";
        return Status::Failed;
    }

    // Minimum wait after InteractUnit before polling — avoids reading stale
    // questgiver arrays that persist from a previous interaction with this NPC.
    if (now - m_gossipWaitStart < kGossipMinWaitMs)
        return Status::Ongoing;

    // Check gossip API (GOSSIP_SHOW)
    int gossipAvail  = game::lua::GetInt("GetNumGossipAvailableQuests()");
    int gossipActive = game::lua::GetInt("GetNumGossipActiveQuests()");

    if (!m_firstPollLogged) {
        m_firstPollLogged = true;
        int greetAvailDbg  = game::lua::GetInt("GetNumAvailableQuests()");
        int greetActiveDbg = game::lua::GetInt("GetNumActiveQuests()");
        LOG(INFO) << "[QuestApproach] First poll (+" << (now - m_gossipWaitStart) << "ms)"
                  << " gossipAvail=" << gossipAvail << " gossipActive=" << gossipActive
                  << " gossipWindowOpen=" << game::quest::IsGossipWindowOpen()
                  << " gossipObjGUID=0x" << std::hex << game::mem::ReadU64(offsets::gossip::ObjectGUID) << std::dec
                  << " greetAvail=" << greetAvailDbg << " greetActive=" << greetActiveDbg;
    }

    if (gossipAvail + gossipActive > 0) {
        m_useGreeting = false;
        LOG(INFO) << "[QuestApproach] Gossip open (" << gossipAvail << " avail, " << gossipActive << " active)";
        return Status::GossipOpen;
    }

    // Check if gossip window is open with 0 quests (vendor/flightmaster/etc with quest)
    if (game::quest::IsGossipWindowOpen()) {
        m_useGreeting = false;
        return Status::GossipOpen;
    }

    // Check QUEST_GREETING API
    int greetAvail  = game::lua::GetInt("GetNumAvailableQuests()");
    int greetActive = game::lua::GetInt("GetNumActiveQuests()");

    if (greetAvail + greetActive > 0) {
        m_useGreeting = true;
        LOG(INFO) << "[QuestApproach] QUEST_GREETING open (" << greetAvail << " avail, " << greetActive << " active)";
        return Status::GossipOpen;
    }

    // Server may auto-select the quest and skip straight to offer-reward / request-items
    // frame without sending QUEST_GREETING (happens when NPC has exactly one quest for player).
    // GetTitleText() is populated by SMSG_QUESTGIVER_OFFER_REWARD and SMSG_QUESTGIVER_REQUEST_ITEMS.
    std::string titleText = game::lua::GetValue("GetTitleText()");
    if (!titleText.empty() && titleText != "nil") {
        m_useGreeting = true;
        m_questFrameOpen = true;
        LOG(INFO) << "[QuestApproach] Quest frame auto-opened by server, title=\"" << titleText << "\"";
        return Status::GossipOpen;
    }

    return Status::Ongoing;
}

void QuestApproachHelper::Abort()
{
    if (m_useNav)
        m_nav.Stop();
    else
        game::movement::StopCTM();
    m_useNav = false;
}

} // namespace bot
