#include "local_player.h"
#include "../offsets/offsets.h"
#include "mem.h"

#define NOMINMAX
#include <Windows.h>

namespace game {

uint32_t LocalPlayer::GetXP() const
{
    return GetDescU32(offsets::fields::PLAYER_XP);
}

uint32_t LocalPlayer::GetNextLevelXP() const
{
    return GetDescU32(offsets::fields::PLAYER_NEXT_LEVEL_XP);
}

uint32_t LocalPlayer::GetCoinage() const
{
    return GetDescU32(offsets::fields::PLAYER_COINAGE);
}

uint32_t LocalPlayer::GetGold() const
{
    return GetCoinage() / 10000;
}

uint32_t LocalPlayer::GetStrength() const
{
    return GetDescU32(offsets::fields::UNIT_STAT0);
}

uint32_t LocalPlayer::GetAgility() const
{
    return GetDescU32(offsets::fields::UNIT_STAT1);
}

uint32_t LocalPlayer::GetStamina() const
{
    return GetDescU32(offsets::fields::UNIT_STAT2);
}

uint32_t LocalPlayer::GetIntellect() const
{
    return GetDescU32(offsets::fields::UNIT_STAT3);
}

uint32_t LocalPlayer::GetSpirit() const
{
    return GetDescU32(offsets::fields::UNIT_STAT4);
}

uint8_t LocalPlayer::GetComboPoints() const
{
    return static_cast<uint8_t>(mem::ReadU32(offsets::globals::ComboPoints) & 0xFF);
}

std::string LocalPlayer::GetPlayerName() const
{
    return mem::ReadCString(offsets::globals::PlayerName);
}

GUID LocalPlayer::GetCurrentTargetGUID() const
{
    return mem::ReadU64(offsets::globals::TargetGUID);
}

bool LocalPlayer::HasSpell(uint32_t spellId) const
{
    uintptr_t fn = offsets::fn::IsSpellKnown;
    int result = 0;

    __try {
        __asm {
            push spellId
            call fn
            add esp, 4
            mov result, eax
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }

    return result != 0;
}

} // namespace game
