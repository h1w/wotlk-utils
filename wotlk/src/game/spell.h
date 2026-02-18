#pragma once
// =============================================================================
// Spell -- query and cast spells.
// C++ for queries (HasSpell, cooldown), Lua for actions (cast).
// =============================================================================

#include <cstdint>

namespace game::spell {

// --- C++ queries ---
bool HasSpell(uint32_t spellId);
bool IsOnCooldown(uint32_t spellId);

// --- Lua actions ---
bool CastById(uint32_t spellId);
bool CastByName(const char* name);
bool StopCasting();

} // namespace game::spell
