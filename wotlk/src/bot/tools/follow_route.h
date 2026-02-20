#pragma once
// =============================================================================
// FollowRouteTool — walk through an array of waypoints via ClickToMove.
//
// Supports avoidance of hostile NPCs and forced combat when path is blocked.
// =============================================================================

#include "../tool.h"
#include "../threat_scanner.h"
#include "../movement_synth.h"
#include "../../game/types.h"

#include <vector>
#include <cstdint>

namespace bot {

class FollowRouteTool : public ITool {
public:
    explicit FollowRouteTool(std::vector<game::Vec3> waypoints, bool loop = false,
                             int forcedCombatCount = 0);

    ToolType    GetType() const override   { return ToolType::FollowRoute; }
    const char* GetName() const override   { return "Follow Route"; }
    ToolStatus  GetStatus() const override { return m_status; }

    void Start() override;
    void Tick() override;
    void Abort() override;

    std::string Describe() const override;

    // Public getters for UI / Radar
    size_t GetCurrentWaypointIndex() const { return m_currentIndex; }
    size_t GetTotalWaypoints() const       { return m_waypoints.size(); }
    const std::vector<game::Vec3>& GetWaypoints() const { return m_waypoints; }
    float GetDistanceToCurrentWP() const;
    float GetProgress() const;

    // Avoidance state for radar
    const std::vector<game::Vec3>& GetDetourWaypoints() const { return m_detourWaypoints; }
    bool IsInForcedCombat() const { return m_inForcedCombat; }

private:
    std::vector<game::Vec3> m_waypoints;
    bool       m_loop = false;
    size_t     m_currentIndex = 0;
    ToolStatus m_status = ToolStatus::Pending;

    // Stuck detection
    game::Vec3 m_lastPosition;
    uint64_t   m_lastStuckCheckTick = 0;
    uint32_t   m_stuckCount = 0;

    // CTM refresh — re-issue CTM periodically so lookahead target stays current
    uint64_t   m_lastCTMTick = 0;

    // Timing
    uint64_t   m_startTick = 0;

    // Avoidance
    int        m_forcedCombatCount = 0;
    bool       m_inForcedCombat = false;
    int        m_currentDangerSegCount = 0;
    std::vector<game::Vec3> m_detourWaypoints;

    // Reroute state — hysteresis, cooldown, rate limiting
    struct RerouteState {
        uint64_t lastRerouteTime   = 0;
        uint64_t lastFallbackCheck = 0;
        int      rerouteCount      = 0;
        uint64_t windowStart       = 0;

        static constexpr uint64_t kMinInterval  = 5000;
        static constexpr uint64_t kFallbackMs   = 8000;
        static constexpr int      kMaxPerWindow = 5;
        static constexpr uint64_t kWindowMs     = 10000;
        static constexpr float    kHysteresis   = 0.15f;
    };
    RerouteState m_reroute;

    // Movement humanization
    MovementSynth m_synth;
    bool          m_inMicroPause = false;
    uint64_t      m_microPauseEnd = 0;

    static constexpr float    kLookaheadDist      = 12.0f;
    static constexpr float    kArrivalThreshold   = 2.5f;
    static constexpr float    kStuckThreshold     = 1.0f;
    static constexpr uint32_t kStuckCheckMs       = 3000;
    static constexpr uint32_t kCTMRefreshMs       = 1000;  // re-issue CTM every 1s
    static constexpr uint32_t kMaxStuckRetries    = 5;
    static constexpr uint32_t kTimeoutMs          = 300000; // 5 minutes
    static constexpr int      kMaxForcedCombats   = 5;

    void AdvanceToNext();
    void IssueCTMToCurrentWP();    // Full: lookahead + noise + snap (initial, advance, reroute)
    void RefreshCTM();             // Light: raw lookahead only (periodic refresh)
    void CheckThreatsAndReroute();
    void HandleBlockedPath(const std::vector<BlockingThreat>& threats);
    bool ShouldReroute(uint64_t now) const;
};

} // namespace bot
