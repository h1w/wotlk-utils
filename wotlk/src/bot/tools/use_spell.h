#pragma once
// =============================================================================
// UseSpellTool — cast a single spell by ID.
//
// Waits for cooldown if on CD (up to a timeout), casts, waits for cast to
// finish. Completes when cast is done. Fails if spell unknown, timeout, etc.
// =============================================================================

#include "../tool.h"
#include <cstdint>

namespace bot {

class UseSpellTool : public ITool {
public:
    explicit UseSpellTool(uint32_t spellId);

    ToolType    GetType() const override   { return ToolType::UseSpell; }
    const char* GetName() const override   { return "Use Spell"; }
    ToolStatus  GetStatus() const override { return m_status; }

    void Start() override;
    void Tick() override;
    void Abort() override;

    std::string Describe() const override;

    uint32_t GetSpellId() const { return m_spellId; }

private:
    enum class Phase : uint8_t {
        WaitCooldown,  // Waiting for spell to come off cooldown
        Casting,       // CastById issued, waiting for cast to finish
        Done,
    };

    uint32_t   m_spellId;
    ToolStatus m_status = ToolStatus::Pending;
    Phase      m_phase  = Phase::WaitCooldown;
    uint64_t   m_startTick = 0;
    bool       m_castIssued = false;

    static constexpr uint32_t kCooldownTimeoutMs = 10000;  // Max wait for CD
    static constexpr uint32_t kCastTimeoutMs     = 15000;  // Max time for cast to finish
};

} // namespace bot
