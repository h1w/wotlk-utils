#pragma once
// =============================================================================
// WaitTool — waits for a specified duration (milliseconds).
// Simplest tool implementation, useful for testing the queue.
// =============================================================================

#include "../tool.h"
#include <cstdint>

namespace bot {

class WaitTool : public ITool {
public:
    explicit WaitTool(uint32_t durationMs);

    ToolType    GetType() const override   { return ToolType::Wait; }
    const char* GetName() const override   { return "Wait"; }
    ToolStatus  GetStatus() const override { return m_status; }

    void Start() override;
    void Tick() override;
    void Abort() override;

    std::string Describe() const override;

    uint32_t GetDurationMs() const   { return m_durationMs; }
    uint64_t GetElapsedMs() const;
    float    GetProgress() const;    // 0.0 .. 1.0

private:
    uint32_t   m_durationMs;
    uint64_t   m_startTick = 0;
    ToolStatus m_status = ToolStatus::Pending;
};

} // namespace bot
