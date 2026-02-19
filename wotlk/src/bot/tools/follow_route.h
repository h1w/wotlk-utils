#pragma once
// =============================================================================
// FollowRouteTool — walk through an array of waypoints via ClickToMove.
// =============================================================================

#include "../tool.h"
#include "../../game/types.h"

#include <vector>
#include <cstdint>

namespace bot {

class FollowRouteTool : public ITool {
public:
    explicit FollowRouteTool(std::vector<game::Vec3> waypoints, bool loop = false);

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

private:
    std::vector<game::Vec3> m_waypoints;
    bool       m_loop = false;
    size_t     m_currentIndex = 0;
    ToolStatus m_status = ToolStatus::Pending;

    // Stuck detection
    game::Vec3 m_lastPosition;
    uint64_t   m_lastStuckCheckTick = 0;
    uint32_t   m_stuckCount = 0;

    // Timing
    uint64_t   m_startTick = 0;

    static constexpr float    kArrivalThreshold = 2.5f;
    static constexpr float    kStuckThreshold   = 1.0f;
    static constexpr uint32_t kStuckCheckMs     = 3000;
    static constexpr uint32_t kMaxStuckRetries  = 5;
    static constexpr uint32_t kTimeoutMs        = 300000; // 5 minutes

    void AdvanceToNext();
    void IssueCTMToCurrentWP();
};

} // namespace bot
