#pragma once
#include <cstdint>
#include <cmath>

namespace game {

using GUID = uint64_t;
inline constexpr GUID GUID_NONE = 0;

struct Vec3 {
    float x = 0.f;
    float y = 0.f;
    float z = 0.f;

    float DistanceTo(const Vec3& o) const {
        float dx = x - o.x, dy = y - o.y, dz = z - o.z;
        return std::sqrtf(dx * dx + dy * dy + dz * dz);
    }

    float Distance2D(const Vec3& o) const {
        float dx = x - o.x, dy = y - o.y;
        return std::sqrtf(dx * dx + dy * dy);
    }
};

enum class ObjectType : int {
    Object      = 0,
    Item        = 1,
    Container   = 2,
    Unit        = 3,
    Player      = 4,
    GameObject  = 5,
    DynObject   = 6,
    Corpse      = 7,
    Max         = 8
};

enum class PowerType : int {
    Mana       = 0,
    Rage       = 1,
    Focus      = 2,
    Energy     = 3,
    Happiness  = 4,
    Runes      = 5,
    RunicPower = 6,
    Max        = 7
};

enum class UnitReaction : int {
    Hostile     = 0,
    Unfriendly  = 1,
    Neutral     = 2,
    Friendly    = 3,
    Honored     = 4,
    Max         = 5
};

// Unit flags (UNIT_FIELD_FLAGS)
namespace UnitFlags {
    inline constexpr uint32_t InCombat        = 0x00080000;
    inline constexpr uint32_t Stunned         = 0x00040000;
    inline constexpr uint32_t Pacified        = 0x00020000;
    inline constexpr uint32_t Confused        = 0x00400000;
    inline constexpr uint32_t Fleeing         = 0x00800000;
    inline constexpr uint32_t NotAttackable   = 0x00000002;
    inline constexpr uint32_t PlayerControlled = 0x01000000;
    inline constexpr uint32_t Looting         = 0x00000400;
    inline constexpr uint32_t Skinnable       = 0x04000000;
}

// Dynamic flags (UNIT_DYNAMIC_FLAGS)
namespace DynFlags {
    inline constexpr uint32_t Lootable        = 0x0001;
    inline constexpr uint32_t Dead            = 0x0020;
    inline constexpr uint32_t Tapped          = 0x0004;
    inline constexpr uint32_t TappedByPlayer  = 0x0008;
}

} // namespace game
