#pragma once
// =============================================================================
// Tool — base abstraction for multi-frame bot actions.
//
// Each tool runs over multiple frames: Start() once, Tick() every frame,
// Abort() to cancel. Tools are executed by ActionQueue one at a time.
// =============================================================================

#include <cstdint>
#include <string>
#include <memory>

namespace bot {

enum class ToolType : uint8_t {
    MoveTo,
    Attack,
    CombatRotation,
    FollowRoute,
    Interact,
    Loot,
    UseSpell,
    Wait,
    Sequence,
    StrategicNav,
};

enum class ToolStatus : uint8_t {
    Pending,      // In queue, not started yet
    Running,      // Start() called, Tick() called each frame
    Completed,    // Finished successfully
    Failed,       // Could not complete
    Cancelled,    // Aborted externally
};

class ITool {
public:
    virtual ~ITool() = default;

    // --- Identity ---
    virtual ToolType    GetType() const = 0;
    virtual const char* GetName() const = 0;

    // --- State ---
    virtual ToolStatus  GetStatus() const = 0;

    // --- Lifecycle ---
    virtual void Start() = 0;   // Called once: Pending -> Running
    virtual void Tick() = 0;    // Called every frame while Running
    virtual void Abort() = 0;   // Cancel: Running -> Cancelled

    // --- UI / Debug ---
    virtual std::string Describe() const = 0;
};

// Convenience alias
using ToolPtr = std::unique_ptr<ITool>;

} // namespace bot
