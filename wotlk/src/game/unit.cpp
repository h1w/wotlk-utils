#include "unit.h"
#include "../offsets/offsets.h"
#include "mem.h"

#define NOMINMAX
#include <Windows.h>

namespace game {

// ---------------------------------------------------------------------------
// Health / Power
// ---------------------------------------------------------------------------

uint32_t Unit::GetHealth() const
{
    return GetDescU32(offsets::fields::UNIT_HEALTH);
}

uint32_t Unit::GetMaxHealth() const
{
    return GetDescU32(offsets::fields::UNIT_MAXHEALTH);
}

float Unit::GetHealthPercent() const
{
    uint32_t max = GetMaxHealth();
    if (max == 0) return 0.f;
    return static_cast<float>(GetHealth()) / static_cast<float>(max) * 100.f;
}

uint32_t Unit::GetPower(PowerType type) const
{
    int idx = static_cast<int>(type);
    if (idx < 0 || idx >= static_cast<int>(PowerType::Max)) return 0;
    return GetDescU32(offsets::fields::UNIT_POWER1 + idx);
}

uint32_t Unit::GetMaxPower(PowerType type) const
{
    int idx = static_cast<int>(type);
    if (idx < 0 || idx >= static_cast<int>(PowerType::Max)) return 0;
    return GetDescU32(offsets::fields::UNIT_MAXPOWER1 + idx);
}

float Unit::GetPowerPercent(PowerType type) const
{
    uint32_t max = GetMaxPower(type);
    if (max == 0) return 0.f;
    return static_cast<float>(GetPower(type)) / static_cast<float>(max) * 100.f;
}

// ---------------------------------------------------------------------------
// Info
// ---------------------------------------------------------------------------

uint32_t Unit::GetLevel() const
{
    return GetDescU32(offsets::fields::UNIT_LEVEL);
}

GUID Unit::GetTargetGUID() const
{
    return GetDescU64(offsets::fields::UNIT_TARGET);
}

uint32_t Unit::GetFactionTemplate() const
{
    return GetDescU32(offsets::fields::UNIT_FACTIONTEMPLATE);
}

uint32_t Unit::GetUnitFlags() const
{
    return GetDescU32(offsets::fields::UNIT_FLAGS);
}

uint32_t Unit::GetDisplayId() const
{
    return GetDescU32(offsets::fields::UNIT_DISPLAYID);
}

uint32_t Unit::GetNativeDisplayId() const
{
    return GetDescU32(offsets::fields::UNIT_NATIVEDISPLAYID);
}

uint32_t Unit::GetMountDisplayId() const
{
    return GetDescU32(offsets::fields::UNIT_MOUNTDISPLAYID);
}

// ---------------------------------------------------------------------------
// Name
// ---------------------------------------------------------------------------

std::string Unit::GetUnitName() const
{
    // First try VTable GetName
    std::string name = GetName();
    if (!name.empty()) return name;

    // Fallback for NPCs: ptr+0x964 -> +0x5C -> const char*
    uintptr_t nameStruct = mem::ReadPointer(m_ptr + offsets::unit::NameOffset1);
    if (nameStruct == 0) return {};
    uintptr_t nameAddr = mem::ReadPointer(nameStruct + offsets::unit::NameOffset2);
    if (nameAddr == 0) return {};
    return mem::ReadCString(nameAddr);
}

// ---------------------------------------------------------------------------
// Reaction
// ---------------------------------------------------------------------------

UnitReaction Unit::GetReaction(const Unit& other) const
{
    uintptr_t fn = offsets::fn::UnitReaction;
    uintptr_t thisPtr = m_ptr;
    uintptr_t otherPtr = other.Ptr();
    int result = 0;

    __try {
        __asm {
            push otherPtr
            mov ecx, thisPtr
            call fn
            mov result, eax
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return UnitReaction::Neutral;
    }

    if (result < 0 || result >= static_cast<int>(UnitReaction::Max))
        return UnitReaction::Neutral;
    return static_cast<UnitReaction>(result);
}

bool Unit::IsHostile(const Unit& other) const
{
    return GetReaction(other) <= UnitReaction::Unfriendly;
}

bool Unit::IsFriendly(const Unit& other) const
{
    return GetReaction(other) >= UnitReaction::Friendly;
}

// ---------------------------------------------------------------------------
// Auras
// ---------------------------------------------------------------------------

bool Unit::HasAura(uint32_t spellId) const
{
    uintptr_t fn = offsets::fn::HasAuraBySpellId;
    uintptr_t thisPtr = m_ptr;
    int result = 0;

    __try {
        __asm {
            push spellId
            mov ecx, thisPtr
            call fn
            mov result, eax
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }

    return result != 0;
}

// ---------------------------------------------------------------------------
// Casting
// ---------------------------------------------------------------------------

uint32_t Unit::GetCastingSpellId() const
{
    return mem::ReadU32(m_ptr + offsets::unit::CastingSpellId);
}

uint32_t Unit::GetChanneledSpellId() const
{
    return mem::ReadU32(m_ptr + offsets::unit::ChanneledSpellId);
}

bool Unit::IsCasting() const
{
    return GetCastingSpellId() != 0 || GetChanneledSpellId() != 0;
}

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

bool Unit::IsDead() const
{
    return GetHealth() == 0;
}

bool Unit::IsPlayer() const
{
    return GetType() == ObjectType::Player;
}

bool Unit::InCombat() const
{
    return (GetUnitFlags() & UnitFlags::InCombat) != 0;
}

} // namespace game
