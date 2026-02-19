#pragma once
// =============================================================================
// MoveToTool — walk to a target position using ClickToMove.
//
// Arrival: distance < threshold. Stuck detection: if no progress for N seconds,
// re-issues ClickToMove. After max retries → Failed.
// =============================================================================

#include "../tool.h"
#include "../../game/types.h"

namespace bot {

class MoveToTool : public ITool {
public:
    explicit MoveToTool(const game::Vec3& target, float arrivalDist = 3.0f);

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
    void IssueCTM();

    game::Vec3 m_target;
    float      m_arrivalDist;
    ToolStatus m_status = ToolStatus::Pending;

    // Stuck detection
    game::Vec3 m_lastPos{};
    uint64_t   m_lastProgressTick = 0;
    int        m_retries = 0;

    static constexpr int    kMaxRetries       = 5;
    static constexpr float  kStuckThreshold   = 1.0f;   // yards — must move this much
    static constexpr uint32_t  kStuckTimeoutMs   = 3000;   // ms without progress → retry
    static constexpr uint32_t  kRetryIntervalMs  = 1000;   // ms between CTM re-issues
};

} // namespace bot
