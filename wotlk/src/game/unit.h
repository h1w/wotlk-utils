#pragma once
// =============================================================================
// Unit -- extends WowObject with HP, power, level, target, auras, casting.
// Covers both NPCs (ObjectType::Unit) and players (ObjectType::Player).
// =============================================================================

#include "game_object.h"

namespace game {

class Unit : public WowObject {
public:
    explicit Unit(uintptr_t ptr) : WowObject(ptr) {}

    // --- Health / Power ---
    uint32_t GetHealth() const;
    uint32_t GetMaxHealth() const;
    float    GetHealthPercent() const;
    uint32_t GetPower(PowerType type = PowerType::Mana) const;
    uint32_t GetMaxPower(PowerType type = PowerType::Mana) const;
    float    GetPowerPercent(PowerType type = PowerType::Mana) const;

    // --- Info ---
    uint32_t GetLevel() const;
    GUID     GetTargetGUID() const;
    uint32_t GetFactionTemplate() const;
    uint32_t GetUnitFlags() const;
    uint32_t GetDisplayId() const;
    uint32_t GetNativeDisplayId() const;
    uint32_t GetMountDisplayId() const;

    // --- Name (NPC fallback via +0x964 -> +0x5C) ---
    std::string GetUnitName() const;

    // --- Reaction ---
    UnitReaction GetReaction(const Unit& other) const;
    bool IsHostile(const Unit& other) const;
    bool IsFriendly(const Unit& other) const;

    // --- Auras ---
    bool HasAura(uint32_t spellId) const;

    // --- Casting ---
    uint32_t GetCastingSpellId() const;
    uint32_t GetChanneledSpellId() const;
    bool     IsCasting() const;

    // --- State ---
    bool IsDead() const;
    bool IsPlayer() const;
    bool InCombat() const;
};

} // namespace game
