#include "action_queue.h"
#include "../game/movement.h"
#include <glog/logging.h>

namespace bot {

ActionQueue& ActionQueue::Instance()
{
    static ActionQueue instance;
    return instance;
}

void ActionQueue::ApplyPendingInterrupt()
{
    if (!m_pendingInterrupt)
        return;

    if (!m_queue.empty()) {
        auto& front = m_queue.front();
        std::string desc = front->Describe();
        if (front->GetStatus() == ToolStatus::Running)
            front->Abort();
        LOG(INFO) << "[BOT] Interrupted: " << desc;
        m_queue.pop_front();
    }

    m_queue.push_front(std::move(m_pendingInterrupt));
    m_pendingInterrupt.reset();
}

void ActionQueue::Tick()
{
    // Apply deferred interrupt from previous frame (tool called Interrupt on itself)
    ApplyPendingInterrupt();

    if (m_queue.empty())
        return;

    auto* current = m_queue.front().get();

    // Start if still pending
    if (current->GetStatus() == ToolStatus::Pending) {
        LOG(INFO) << "[BOT] Starting tool: " << current->Describe();
        current->Start();
    }

    // Tick if running (RAII guard ensures m_insideTick is cleared even on exception)
    if (current->GetStatus() == ToolStatus::Running) {
        struct TickGuard {
            bool& flag;
            TickGuard(bool& f) : flag(f) { flag = true; }
            ~TickGuard() { flag = false; }
        } guard(m_insideTick);
        current->Tick();
    }

    // If the tool called Interrupt() during Tick(), it was deferred.
    // Apply it now (safe — we're no longer inside the tool's call stack).
    if (m_pendingInterrupt) {
        ApplyPendingInterrupt();
        return;
    }

    // If the queue changed during Tick (shouldn't happen with deferred, but guard)
    if (m_queue.empty() || m_queue.front().get() != current)
        return;

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

    if (m_insideTick) {
        // Defer: we're inside a tool's Tick() — destroying the current tool now
        // would cause use-after-free when the call stack unwinds through it.
        if (m_pendingInterrupt) {
            LOG(WARNING) << "[BOT] Multiple Interrupt() in single Tick, dropping: "
                         << tool->Describe();
            return;
        }
        m_pendingInterrupt = std::move(tool);
        return;
    }

    // Immediate: called from outside Tick() (e.g., UI button click)
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
    if (index == 0 && tool->GetStatus() == ToolStatus::Running) {
        tool->Abort();
        // Safety net: ensure movement fully stops regardless of tool's Abort impl
        game::movement::StopCTM();
        game::movement::StopMoving();
    }

    LOG(INFO) << "[BOT] Remove[" << index << "]: " << desc;
    m_queue.erase(m_queue.begin() + static_cast<ptrdiff_t>(index));
}

void ActionQueue::Clear()
{
    m_pendingInterrupt.reset();

    if (!m_queue.empty()) {
        auto* current = m_queue.front().get();
        if (current->GetStatus() == ToolStatus::Running) {
            current->Abort();
            // Safety net: ensure movement fully stops
            game::movement::StopCTM();
            game::movement::StopMoving();
        }
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
