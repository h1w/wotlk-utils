#include "wait.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdio>

namespace bot {

WaitTool::WaitTool(uint32_t durationMs)
    : m_durationMs(durationMs)
{
}

void WaitTool::Start()
{
    m_startTick = GetTickCount64();
    m_status = ToolStatus::Running;
}

void WaitTool::Tick()
{
    if (m_status != ToolStatus::Running)
        return;

    if (GetElapsedMs() >= m_durationMs)
        m_status = ToolStatus::Completed;
}

void WaitTool::Abort()
{
    if (m_status == ToolStatus::Running || m_status == ToolStatus::Pending)
        m_status = ToolStatus::Cancelled;
}

std::string WaitTool::Describe() const
{
    char buf[64];
    snprintf(buf, sizeof(buf), "Wait (%u ms)", m_durationMs);
    return buf;
}

uint64_t WaitTool::GetElapsedMs() const
{
    if (m_startTick == 0)
        return 0;
    return GetTickCount64() - m_startTick;
}

float WaitTool::GetProgress() const
{
    if (m_durationMs == 0)
        return 1.0f;
    float p = static_cast<float>(GetElapsedMs()) / static_cast<float>(m_durationMs);
    return (p > 1.0f) ? 1.0f : p;
}

} // namespace bot
