#pragma once
// =============================================================================
// NavHelper — reusable navmesh pathfinding + waypoint follower.
//
// Computes a path via Pathfinder::FindPath(), then walks waypoints via CTM
// with stuck detection. Extracted from FollowRouteTool logic.
// =============================================================================

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

private:
    std::vector<game::Vec3> m_waypoints;
    size_t   m_currentIndex = 0;
    Status   m_status = Status::Idle;

    // Stuck detection
    game::Vec3 m_lastPosition{};
    uint64_t   m_lastStuckCheckTick = 0;
    uint32_t   m_stuckCount = 0;

    static constexpr float    kArrivalThreshold = 2.5f;
    static constexpr float    kStuckThreshold   = 1.0f;
    static constexpr uint32_t kStuckCheckMs     = 3000;
    static constexpr uint32_t kMaxStuckRetries  = 5;

    void AdvanceToNext();
    void IssueCTMToCurrentWP();
};

} // namespace bot
