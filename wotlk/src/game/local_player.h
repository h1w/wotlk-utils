#pragma once
// =============================================================================
// LocalPlayer -- extends Unit with player-specific data (XP, gold, stats).
// =============================================================================

#include "unit.h"

namespace game {

class LocalPlayer : public Unit {
public:
    explicit LocalPlayer(uintptr_t ptr) : Unit(ptr) {}

    // --- XP / Gold ---
    uint32_t GetXP() const;
    uint32_t GetNextLevelXP() const;
    uint32_t GetCoinage() const;    // in copper
    uint32_t GetGold() const;       // coinage / 10000

    // --- Stats ---
    uint32_t GetStrength() const;
    uint32_t GetAgility() const;
    uint32_t GetStamina() const;
    uint32_t GetIntellect() const;
    uint32_t GetSpirit() const;

    // --- Combo points (global) ---
    uint8_t GetComboPoints() const;

    // --- Player name (global) ---
    std::string GetPlayerName() const;

    // --- Target GUID (global, not descriptor) ---
    GUID GetCurrentTargetGUID() const;

    // --- Spellbook ---
    bool HasSpell(uint32_t spellId) const;
};

} // namespace game
