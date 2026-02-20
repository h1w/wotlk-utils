#pragma once
// =============================================================================
// ActionQueue — deque-based sequential executor for bot tools.
//
// One tool runs at a time (the front of the deque). Each frame, EndScene calls
// Tick() which advances the current tool. Tools can be pushed to the back,
// inserted next, or used to interrupt the current action.
// =============================================================================

#include "tool.h"
#include <deque>

namespace bot {

// NOTE: ActionQueue is ONLY accessed from the main game thread (EndScene hook).
// Future IPC (pipe server) will use a separate thread-safe command buffer;
// the main thread drains it inside Tick(). No mutex needed here.
class ActionQueue {
public:
    // --- Singleton ---
    static ActionQueue& Instance();

    // --- Frame tick (called from EndScene, main thread only) ---
    void Tick();

    // --- Queue operations ---
    void PushBack(ToolPtr tool);     // Add to end
    void Interrupt(ToolPtr tool);    // Abort current + push to front
    void InsertNext(ToolPtr tool);   // Insert at position 1 (after current)
    void Remove(size_t index);       // Remove by index
    void Clear();                    // Abort current + clear all

    // --- Queries ---
    ITool*       GetCurrent() const;
    size_t       Size() const;
    bool         IsEmpty() const;

    const std::deque<ToolPtr>& GetAll() const { return m_queue; }

private:
    ActionQueue() = default;

    // Apply any pending deferred interrupt (called at start of Tick)
    void ApplyPendingInterrupt();

    std::deque<ToolPtr> m_queue;

    // Deferred interrupt: stored here when a tool calls Interrupt() during its
    // own Tick(). Applied at the start of the next Tick() to avoid use-after-free
    // (a tool calling Interrupt destroys itself via pop_front, then the call
    // stack unwinds through the destroyed object's methods).
    ToolPtr m_pendingInterrupt;
    bool    m_insideTick = false;
};

} // namespace bot
