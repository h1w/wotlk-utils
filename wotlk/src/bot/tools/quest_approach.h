#pragma once
// =============================================================================
// QuestApproachHelper — shared Approaching + WaitGossip phases for quest tools.
//
// All 4 quest tools (QueryQuests, AcceptQuest, QueryRewards, CompleteQuest)
// share the same NPC approach + gossip-open detection logic. This helper
// encapsulates that logic via composition.
//
// Usage:
//   1. Call Init(npcGuid) from the tool's Start().
//   2. Call Tick() every frame until it returns GossipOpen or Failed.
//   3. Use UseGreeting() to know which Lua API set to use afterward.
// =============================================================================

#include "../nav_helper.h"
#include "../../game/types.h"

#include <cstdint>
#include <string>

namespace bot {

class QuestApproachHelper {
public:
    enum class Status : uint8_t {
        Ongoing,      // Still approaching or waiting for gossip
        GossipOpen,   // Gossip or QUEST_GREETING window is open
        Failed,       // Timeout, NPC gone, or other failure
    };

    // Called once from the tool's Start(). Returns false if the NPC doesn't exist.
    bool Init(game::GUID npcGuid);

    // Called every frame while Status == Ongoing.
    Status Tick();

    // Cancel navigation and stop CTM.
    void Abort();

    // True if the quest window was opened as QUEST_GREETING (not GOSSIP_SHOW).
    // Valid only after Tick() returns GossipOpen.
    bool UseGreeting() const { return m_useGreeting; }

    // True if the server auto-selected the quest and jumped straight to the
    // offer-reward / request-items frame (no QUEST_GREETING quest list step).
    // When this is true, FindAndSelectActive must be skipped.
    bool IsQuestFrameOpen() const { return m_questFrameOpen; }

    const std::string& GetNpcName() const { return m_npcName; }
    const game::Vec3&  GetNpcPos()  const { return m_npcPos; }

private:
    enum class Phase : uint8_t {
        Approaching,
        WaitGossip,
    };

    game::GUID  m_npcGuid          = 0;
    std::string m_npcName;
    game::Vec3  m_npcPos{};

    NavHelper   m_nav;
    bool        m_useNav           = false;
    bool        m_interactionIssued= false;
    bool        m_useGreeting      = false;
    bool        m_alreadyOpen      = false;
    bool        m_firstPollLogged  = false;
    bool        m_questFrameOpen   = false;  // server auto-selected quest, already at offer/progress frame
    Phase       m_phase            = Phase::Approaching;

    uint64_t    m_startTick        = 0;
    uint64_t    m_gossipWaitStart  = 0;

    static constexpr float    kNavSwitchRange    = 10.0f;
    static constexpr float    kInteractRange     = 5.0f;
    static constexpr uint32_t kApproachTimeoutMs = 15000;
    static constexpr uint32_t kGossipTimeoutMs   = 5000;
    static constexpr uint32_t kGossipMinWaitMs   = 300;
};

} // namespace bot
