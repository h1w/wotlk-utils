#pragma once
// =============================================================================
// MoveToTool — walk to a target position using navmesh pathfinding.
//
// Tries NavHelper (navmesh path) first. Falls back to direct ClickToMove
// if navmesh is unavailable or pathfinding fails.
//
// When avoidance is enabled and path is blocked by unavoidable hostiles,
// initiates forced combat (Attack + Loot) then resumes navigation.
// =============================================================================

#include "../tool.h"
#include "../nav_helper.h"
#include "../../game/types.h"
#include "../../navigation/corridor_loader.h"

namespace bot {

class MoveToTool : public ITool {
public:
    explicit MoveToTool(const game::Vec3& target, int forcedCombatCount = 0);

    // Factory: creates a StrategicNavTool if target is far and world graph
    // is available, otherwise returns a plain MoveToTool.
    static ToolPtr CreateSmart(const game::Vec3& target);

    ToolType    GetType() const override   { return ToolType::MoveTo; }
    const char* GetName() const override   { return "Move To"; }
    ToolStatus  GetStatus() const override { return m_status; }

    void Start() override;
    void Tick() override;
    void Abort() override;

    std::string Describe() const override;

    const game::Vec3& GetTarget() const { return m_target; }
    float GetDistanceRemaining() const;

    // Expose nav path for radar visualization
    const std::vector<game::Vec3>& GetNavWaypoints() const { return m_nav.GetWaypoints(); }
    size_t GetNavCurrentIndex() const { return m_nav.GetCurrentWaypointIndex(); }

    // Avoidance state for radar
    const std::vector<game::Vec3>& GetDetourWaypoints() const { return m_nav.GetDetourWaypoints(); }
    bool IsInForcedCombat() const { return m_inForcedCombat; }
    int  GetForcedCombatCount() const { return m_forcedCombatCount; }

private:
    game::Vec3 m_target;
    ToolStatus m_status = ToolStatus::Pending;
    bool       m_useNav = false;

    NavHelper          m_nav;
    nav::CorridorLoader m_corridor;

    // Forced combat
    int  m_forcedCombatCount = 0;
    bool m_inForcedCombat = false;

    // Direct CTM fallback — stuck detection
    game::Vec3 m_lastPos{};
    uint64_t   m_lastProgressTick = 0;
    int        m_retries = 0;

    static constexpr float     kCorridorMinDist     = 1200.0f; // pre-load tiles if >1200yd
    static constexpr float     kArrivalDist         = 3.0f;
    static constexpr int       kMaxRetries          = 5;
    static constexpr float     kStuckThreshold      = 1.0f;
    static constexpr uint32_t  kStuckTimeoutMs      = 3000;
    static constexpr int       kMaxForcedCombats     = 5;

    void StartDirectCTM();
    void HandleBlockedPath();
};

} // namespace bot
