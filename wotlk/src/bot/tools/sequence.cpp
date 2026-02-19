#include "sequence.h"
#include <cstdio>

namespace bot {

SequenceTool::SequenceTool(std::vector<ToolPtr> steps)
    : m_steps(std::move(steps))
{
}

void SequenceTool::Start()
{
    if (m_steps.empty()) {
        m_status = ToolStatus::Completed;
        return;
    }

    m_currentStep = 0;
    m_status = ToolStatus::Running;
    m_steps[0]->Start();
}

void SequenceTool::Tick()
{
    if (m_status != ToolStatus::Running)
        return;

    if (m_currentStep >= m_steps.size()) {
        m_status = ToolStatus::Completed;
        return;
    }

    auto* step = m_steps[m_currentStep].get();

    // Tick current step
    if (step->GetStatus() == ToolStatus::Running)
        step->Tick();

    auto st = step->GetStatus();

    if (st == ToolStatus::Failed) {
        // Step failed → whole sequence fails
        m_status = ToolStatus::Failed;
        return;
    }

    if (st == ToolStatus::Completed) {
        // Advance to next step
        m_currentStep++;
        if (m_currentStep >= m_steps.size()) {
            m_status = ToolStatus::Completed;
        } else {
            m_steps[m_currentStep]->Start();
        }
    }
}

void SequenceTool::Abort()
{
    if (m_status == ToolStatus::Running || m_status == ToolStatus::Pending) {
        if (m_currentStep < m_steps.size()) {
            auto* step = m_steps[m_currentStep].get();
            if (step->GetStatus() == ToolStatus::Running)
                step->Abort();
        }
        m_status = ToolStatus::Cancelled;
    }
}

std::string SequenceTool::Describe() const
{
    char buf[128];
    if (m_steps.empty()) {
        snprintf(buf, sizeof(buf), "Sequence (empty)");
    } else if (m_currentStep < m_steps.size()) {
        snprintf(buf, sizeof(buf), "Sequence [%zu/%zu]: %s",
            m_currentStep + 1, m_steps.size(),
            m_steps[m_currentStep]->Describe().c_str());
    } else {
        snprintf(buf, sizeof(buf), "Sequence [%zu/%zu]: done",
            m_steps.size(), m_steps.size());
    }
    return buf;
}

ITool* SequenceTool::GetCurrentStep() const
{
    if (m_currentStep < m_steps.size())
        return m_steps[m_currentStep].get();
    return nullptr;
}

} // namespace bot
