#pragma once
// =============================================================================
// MoveToTool — walk to a target position using navmesh pathfinding.
//
// Tries NavHelper (navmesh path) first. Falls back to direct ClickToMove
// if navmesh is unavailable or pathfinding fails.
// =============================================================================

#include "../tool.h"
#include "../nav_helper.h"
#include "../../game/types.h"

namespace bot {

class MoveToTool : public ITool {
public:
    explicit MoveToTool(const game::Vec3& target);

    ToolType    GetType() const override   { return ToolType::MoveTo; }
    const char* GetName() const override   { return "Move To"; }
    ToolStatus  GetStatus() const override { return m_status; }

    void Start() override;
    void Tick() override;
    void Abort() override;

    std::string Describe() const override;

    const game::Vec3& GetTarget() const { return m_target; }
    float GetDistanceRemaining() const;

private:
    game::Vec3 m_target;
    ToolStatus m_status = ToolStatus::Pending;
    bool       m_useNav = false;

    NavHelper  m_nav;

    // Direct CTM fallback — stuck detection
    game::Vec3 m_lastPos{};
    uint64_t   m_lastProgressTick = 0;
    int        m_retries = 0;

    static constexpr float     kArrivalDist      = 3.0f;
    static constexpr int       kMaxRetries       = 5;
    static constexpr float     kStuckThreshold   = 1.0f;
    static constexpr uint32_t  kStuckTimeoutMs   = 3000;

    void StartDirectCTM();
};

} // namespace bot
