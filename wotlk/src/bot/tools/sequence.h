#pragma once
// =============================================================================
// SequenceTool — runs a list of sub-tools one after another.
//
// Completed when ALL steps complete. Failed if ANY step fails.
// Abort cancels the current step and the whole sequence.
// =============================================================================

#include "../tool.h"
#include <vector>

namespace bot {

class SequenceTool : public ITool {
public:
    explicit SequenceTool(std::vector<ToolPtr> steps);

    ToolType    GetType() const override   { return ToolType::Sequence; }
    const char* GetName() const override   { return "Sequence"; }
    ToolStatus  GetStatus() const override { return m_status; }

    void Start() override;
    void Tick() override;
    void Abort() override;

    std::string Describe() const override;

    // --- Queries ---
    size_t GetStepCount() const       { return m_steps.size(); }
    size_t GetCurrentStepIndex() const { return m_currentStep; }
    ITool* GetCurrentStep() const;

private:
    std::vector<ToolPtr> m_steps;
    size_t     m_currentStep = 0;
    ToolStatus m_status = ToolStatus::Pending;
};

} // namespace bot
