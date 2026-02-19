#include "action_queue.h"
#include <glog/logging.h>

namespace bot {

ActionQueue& ActionQueue::Instance()
{
    static ActionQueue instance;
    return instance;
}

void ActionQueue::Tick()
{
    if (m_queue.empty())
        return;

    auto* current = m_queue.front().get();

    // Start if still pending
    if (current->GetStatus() == ToolStatus::Pending) {
        LOG(INFO) << "[BOT] Starting tool: " << current->Describe();
        current->Start();
    }

    // Tick if running
    if (current->GetStatus() == ToolStatus::Running)
        current->Tick();

    // Remove if finished
    auto st = current->GetStatus();
    if (st == ToolStatus::Completed || st == ToolStatus::Failed || st == ToolStatus::Cancelled) {
        const char* reason = (st == ToolStatus::Completed) ? "completed"
                           : (st == ToolStatus::Failed)    ? "failed"
                                                           : "cancelled";
        LOG(INFO) << "[BOT] Tool " << reason << ": " << current->Describe();
        m_queue.pop_front();
    }
}

void ActionQueue::PushBack(ToolPtr tool)
{
    LOG(INFO) << "[BOT] PushBack: " << tool->Describe();
    m_queue.push_back(std::move(tool));
}

void ActionQueue::Interrupt(ToolPtr tool)
{
    LOG(INFO) << "[BOT] Interrupt: " << tool->Describe();

    if (!m_queue.empty()) {
        auto& front = m_queue.front();
        std::string desc = front->Describe();
        if (front->GetStatus() == ToolStatus::Running)
            front->Abort();
        LOG(INFO) << "[BOT] Interrupted: " << desc;
        m_queue.pop_front();
    }

    m_queue.push_front(std::move(tool));
}

void ActionQueue::InsertNext(ToolPtr tool)
{
    LOG(INFO) << "[BOT] InsertNext: " << tool->Describe();

    if (m_queue.empty())
        m_queue.push_back(std::move(tool));
    else
        m_queue.insert(m_queue.begin() + 1, std::move(tool));
}

void ActionQueue::Remove(size_t index)
{
    if (index >= m_queue.size())
        return;

    auto* tool = m_queue[index].get();
    std::string desc = tool->Describe();

    // If removing the current running tool, abort it first
    if (index == 0 && tool->GetStatus() == ToolStatus::Running)
        tool->Abort();

    LOG(INFO) << "[BOT] Remove[" << index << "]: " << desc;
    m_queue.erase(m_queue.begin() + static_cast<ptrdiff_t>(index));
}

void ActionQueue::Clear()
{
    if (!m_queue.empty()) {
        auto* current = m_queue.front().get();
        if (current->GetStatus() == ToolStatus::Running)
            current->Abort();
    }

    LOG(INFO) << "[BOT] Clear queue (" << m_queue.size() << " tools)";
    m_queue.clear();
}

ITool* ActionQueue::GetCurrent() const
{
    return m_queue.empty() ? nullptr : m_queue.front().get();
}

size_t ActionQueue::Size() const
{
    return m_queue.size();
}

bool ActionQueue::IsEmpty() const
{
    return m_queue.empty();
}

} // namespace bot
