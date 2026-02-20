#pragma once
// =============================================================================
// NavHelper — reusable navmesh pathfinding + waypoint follower.
//
// Computes a path via Pathfinder::FindPath(), then walks waypoints via CTM
// with stuck detection. Supports avoidance of hostile NPCs via ThreatScanner.
// =============================================================================

#include "threat_scanner.h"
#include "movement_synth.h"
#include "../game/types.h"

#include <vector>
#include <cstdint>

namespace bot {

class NavHelper {
public:
    enum class Status : uint8_t {
        Idle,       // Not navigating
        Moving,     // Following waypoints
        Arrived,    // Reached final waypoint
        Failed,     // Stuck or no path
        Blocked,    // Path blocked by unavoidable hostile NPC(s)
    };

    // Compute path via Pathfinder and start following.
    // Returns false if navmesh not ready or no path found.
    bool StartNavTo(const game::Vec3& target);

    // Tick waypoint following. Call every frame while Status == Moving.
    Status Tick();

    // Stop navigation, issue StopCTM.
    void Stop();

    Status GetStatus() const { return m_status; }
    bool   IsActive() const  { return m_status == Status::Moving; }

    // Current waypoint info (for debug/UI/Radar)
    size_t GetCurrentWaypointIndex() const { return m_currentIndex; }
    size_t GetTotalWaypoints() const       { return m_waypoints.size(); }
    const std::vector<game::Vec3>& GetWaypoints() const { return m_waypoints; }

    // ---- Avoidance ----
    void SetAvoidanceEnabled(bool enabled) { m_avoidanceEnabled = enabled; }
    bool IsAvoidanceEnabled() const        { return m_avoidanceEnabled; }

    // Returns remaining waypoints from current index, prepended with player pos.
    std::vector<game::Vec3> GetRemainingPath() const;

    // Replace waypoints from m_currentIndex onward with newPath.
    void ReplaceRemainingPath(const std::vector<game::Vec3>& newPath);

    // Current blocking threats (valid when Status == Blocked).
    const std::vector<BlockingThreat>& GetBlockingThreats() const { return m_blockingThreats; }
    const BlockingThreat* GetWeakestBlockingThreat() const;

    // Detour waypoints for radar visualization.
    const std::vector<game::Vec3>& GetDetourWaypoints() const { return m_detourWaypoints; }

    // Movement humanization
    void SetHumanizeEnabled(bool enabled) { m_humanize = enabled; }
    bool IsHumanizeEnabled() const        { return m_humanize; }

private:
    std::vector<game::Vec3> m_waypoints;
    size_t   m_currentIndex = 0;
    Status   m_status = Status::Idle;

    // Stuck detection
    game::Vec3 m_lastPosition{};
    uint64_t   m_lastStuckCheckTick = 0;
    uint32_t   m_stuckCount = 0;

    // CTM refresh — re-issue CTM periodically so lookahead target stays current
    uint64_t   m_lastCTMTick = 0;

    // Avoidance
    bool     m_avoidanceEnabled = false;
    bool     m_currentPathThroughDanger = false; // current path has throughDanger
    int      m_currentDangerPolyCount = 0;       // danger polys in current path
    std::vector<BlockingThreat> m_blockingThreats;
    std::vector<game::Vec3>     m_detourWaypoints;

    // Reroute state — hysteresis, cooldown, rate limiting
    struct RerouteState {
        uint64_t lastRerouteTime    = 0;
        uint64_t lastFallbackCheck  = 0;
        int      rerouteCount       = 0;       // count in current 10s window
        uint64_t windowStart        = 0;       // start of rate-limit window

        static constexpr uint64_t kMinInterval    = 5000;   // 5s minimum between reroutes
        static constexpr uint64_t kFallbackMs     = 8000;   // 8s fallback timer
        static constexpr int      kMaxPerWindow   = 5;      // max reroutes in window
        static constexpr uint64_t kWindowMs       = 10000;  // 10s window
        static constexpr float    kHysteresis     = 0.15f;  // 15% improvement required
    };
    RerouteState m_reroute;

    // Recovery protocol — wait → evaluate → fight/flee
    enum class RecoveryState : uint8_t {
        None,            // Not in recovery
        Waiting,         // Waiting for mobs to move (up to 10s)
        Blocked,         // Wait expired, evaluated — setting Status::Blocked
    };
    RecoveryState m_recoveryState = RecoveryState::None;
    uint64_t m_waitStartTime      = 0;
    uint64_t m_lastWaitRecheck    = 0;

    static constexpr uint64_t kRecoveryWaitMs     = 10000;  // wait up to 10s
    static constexpr uint64_t kRecoveryRecheckMs  = 2000;   // re-check every 2s during wait

    // Movement humanization
    bool          m_humanize = false;
    MovementSynth m_synth;
    bool          m_inMicroPause = false;
    uint64_t      m_microPauseEnd = 0;

    // Reaction delay: wait 200-400ms before rerouting when new threats detected
    uint64_t m_reactionDelayEnd = 0;
    bool     m_reactionDelayActive = false;

    static constexpr uint32_t kMinReactionDelayMs  = 200;
    static constexpr uint32_t kMaxReactionDelayMs   = 400;
    static constexpr float    kLookaheadDist       = 12.0f;
    static constexpr float    kArrivalThreshold    = 2.5f;
    static constexpr float    kStuckThreshold      = 1.0f;
    static constexpr uint32_t kStuckCheckMs        = 3000;
    static constexpr uint32_t kCTMRefreshMs        = 1000;  // re-issue CTM every 1s
    static constexpr uint32_t kMaxStuckRetries     = 5;

    void AdvanceToNext();
    void IssueCTMToCurrentWP();    // Full: lookahead + noise + snap (initial, advance, reroute)
    void RefreshCTM();             // Light: raw lookahead only (periodic refresh)
    void CheckThreats();
    bool ShouldReroute(uint64_t now) const;
    void TickRecovery();
};

} // namespace bot
