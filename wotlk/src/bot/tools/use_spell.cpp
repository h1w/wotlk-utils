#include "use_spell.h"
#include "../../game/game.h"
#include "../../game/spell.h"
#include "../../game/world.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdio>

namespace bot {

UseSpellTool::UseSpellTool(uint32_t spellId)
    : m_spellId(spellId)
{
}

void UseSpellTool::Start()
{
    if (!game::world::IsInGame()) {
        m_status = ToolStatus::Failed;
        return;
    }

    if (!game::spell::HasSpell(m_spellId)) {
        m_status = ToolStatus::Failed;
        return;
    }

    m_startTick = GetTickCount64();
    m_status = ToolStatus::Running;

    // If not on cooldown, cast immediately
    if (!game::spell::IsOnCooldown(m_spellId)) {
        game::spell::CastById(m_spellId);
        m_castIssued = true;
        m_phase = Phase::Casting;
        m_startTick = GetTickCount64();
    } else {
        m_phase = Phase::WaitCooldown;
    }
}

void UseSpellTool::Tick()
{
    if (m_status != ToolStatus::Running)
        return;

    uint64_t now = GetTickCount64();

    switch (m_phase) {
    case Phase::WaitCooldown: {
        // Timeout waiting for cooldown?
        if (now - m_startTick > kCooldownTimeoutMs) {
            m_status = ToolStatus::Failed;
            return;
        }

        // Still on cooldown?
        if (game::spell::IsOnCooldown(m_spellId))
            return;

        // Off cooldown — cast now
        game::spell::CastById(m_spellId);
        m_castIssued = true;
        m_phase = Phase::Casting;
        m_startTick = now;
        break;
    }

    case Phase::Casting: {
        // Timeout?
        if (now - m_startTick > kCastTimeoutMs) {
            m_status = ToolStatus::Failed;
            return;
        }

        // Check if player is still casting
        auto player = game::GetLocalPlayer();
        if (!player) {
            m_status = ToolStatus::Failed;
            return;
        }

        // If we issued cast and player is no longer casting → done
        if (m_castIssued && !player->IsCasting()) {
            // Give one frame grace period after CastById before checking
            // (spell may not start instantly)
            if (now - m_startTick > 200) {
                m_status = ToolStatus::Completed;
            }
        }
        break;
    }

    case Phase::Done:
        m_status = ToolStatus::Completed;
        break;
    }
}

void UseSpellTool::Abort()
{
    if (m_status == ToolStatus::Running || m_status == ToolStatus::Pending) {
        if (m_phase == Phase::Casting)
            game::spell::StopCasting();
        m_status = ToolStatus::Cancelled;
    }
}

std::string UseSpellTool::Describe() const
{
    char buf[64];
    snprintf(buf, sizeof(buf), "UseSpell (ID: %u)", m_spellId);
    return buf;
}

} // namespace bot
