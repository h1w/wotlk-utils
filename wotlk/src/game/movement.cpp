#include "movement.h"
#include "../offsets/offsets.h"
#include "object_manager.h"
#include "lua_bridge.h"
#include "mem.h"

#define NOMINMAX
#include <Windows.h>

#include <cmath>

namespace game::movement {

// ClickToMove: void __thiscall(Player*, int action, uint64_t* guid, Vec3* pos, float precision)
static bool CallCTM(int action, GUID guid, const Vec3& pos)
{
    uintptr_t playerPtr = objmgr::GetLocalPlayerPtr();
    if (playerPtr == 0) return false;

    uintptr_t fn = offsets::fn::ClickToMove;
    uint64_t guidVal = guid;
    Vec3 posVal = pos;
    float precision = 0.5f;

    __try {
        __asm {
            push precision
            lea eax, posVal
            push eax
            lea eax, guidVal
            push eax
            push action
            mov ecx, playerPtr
            call fn
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }

    return true;
}

bool ClickToMove(const Vec3& pos)
{
    return CallCTM(offsets::ctm::Move, GUID_NONE, pos);
}

bool ClickToMoveAttack(GUID targetGuid, const Vec3& pos)
{
    return CallCTM(offsets::ctm::Attack, targetGuid, pos);
}

bool ClickToMoveInteract(GUID targetGuid, const Vec3& pos)
{
    return CallCTM(offsets::ctm::Interact, targetGuid, pos);
}

bool ClickToMoveLoot(GUID targetGuid, const Vec3& pos)
{
    return CallCTM(offsets::ctm::Loot, targetGuid, pos);
}

bool StopCTM()
{
    Vec3 dummy;
    return CallCTM(offsets::ctm::Stop, GUID_NONE, dummy);
}

// SetFacing: void __thiscall(Unit*, float facing)
bool SetFacing(float radians)
{
    uintptr_t playerPtr = objmgr::GetLocalPlayerPtr();
    if (playerPtr == 0) return false;

    uintptr_t fn = offsets::fn::SetFacing;

    __try {
        __asm {
            push radians
            mov ecx, playerPtr
            call fn
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }

    return true;
}

bool FacePosition(const Vec3& target)
{
    uintptr_t playerPtr = objmgr::GetLocalPlayerPtr();
    if (playerPtr == 0) return false;

    // Read player position via VTable
    Vec3 myPos;
    uintptr_t vt = mem::ReadPointer(playerPtr);
    if (vt == 0) return false;
    uintptr_t getPosFunc = mem::ReadPointer(vt + offsets::vtable::GetPosition * 4);
    if (getPosFunc == 0) return false;

    __try {
        __asm {
            lea eax, myPos
            push eax
            mov ecx, playerPtr
            call getPosFunc
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }

    float dx = target.x - myPos.x;
    float dy = target.y - myPos.y;
    float angle = std::atan2f(dy, dx);
    if (angle < 0.f)
        angle += 2.f * 3.14159265f;

    return SetFacing(angle);
}

bool Jump()
{
    return lua::Execute("JumpOrAscendStart()");
}

bool StopMoving()
{
    return lua::Execute("MoveAndSteerStop()");
}

} // namespace game::movement
