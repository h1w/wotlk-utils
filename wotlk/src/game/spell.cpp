#include "spell.h"
#include "../offsets/offsets.h"
#include "lua_bridge.h"
#include "mem.h"

#define NOMINMAX
#include <Windows.h>

#include <cstdio>

namespace game::spell {

bool HasSpell(uint32_t spellId)
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

bool IsOnCooldown(uint32_t spellId)
{
    char expr[64];
    snprintf(expr, sizeof(expr), "GetSpellCooldown(%u)", spellId);
    int val = lua::GetInt(expr);
    return val > 0;
}

bool CastById(uint32_t spellId)
{
    return lua::Executef("CastSpellByID(%u)", spellId);
}

bool CastByName(const char* name)
{
    if (!name) return false;
    return lua::Executef("CastSpellByName(\"%s\")", name);
}

bool StopCasting()
{
    return lua::Execute("SpellStopCasting()");
}

} // namespace game::spell
