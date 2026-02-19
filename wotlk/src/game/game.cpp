#include "game.h"
#include "../offsets/offsets.h"
#include "object_manager.h"
#include "mem.h"
#include "world.h"

#define NOMINMAX
#include <Windows.h>
#include <glog/logging.h>

namespace game {

bool Initialize()
{
    LOG(INFO) << "[GAME] SDK initialized";
    return true;
}

void Shutdown()
{
    LOG(INFO) << "[GAME] SDK shutdown";
}

std::optional<LocalPlayer> GetLocalPlayer()
{
    uintptr_t ptr = objmgr::GetLocalPlayerPtr();
    if (ptr == 0) return std::nullopt;
    return LocalPlayer(ptr);
}

std::optional<Unit> GetTarget()
{
    GUID targetGuid = mem::ReadU64(offsets::globals::TargetGUID);
    if (targetGuid == GUID_NONE) return std::nullopt;
    uintptr_t ptr = objmgr::GetObjectPtr(targetGuid);
    if (ptr == 0) return std::nullopt;
    return Unit(ptr);
}

std::optional<Unit> GetMouseOver()
{
    GUID guid = mem::ReadU64(offsets::globals::MouseOverGUID);
    if (guid == GUID_NONE) return std::nullopt;
    uintptr_t ptr = objmgr::GetObjectPtr(guid);
    if (ptr == 0) return std::nullopt;
    return Unit(ptr);
}

std::vector<Unit> GetAllUnits()
{
    std::vector<Unit> units;
    objmgr::EnumObjects([&](uintptr_t objPtr) -> bool {
        ObjectType type = static_cast<ObjectType>(
            mem::ReadU32(objPtr + offsets::objmgr::ObjectType));
        if (type == ObjectType::Unit || type == ObjectType::Player)
            units.emplace_back(objPtr);
        return true;
    });
    return units;
}

std::vector<Unit> GetUnitsInRange(float maxDist)
{
    auto me = GetLocalPlayer();
    if (!me) return {};

    Vec3 myPos = me->GetPosition();
    std::vector<Unit> units;

    objmgr::EnumObjects([&](uintptr_t objPtr) -> bool {
        ObjectType type = static_cast<ObjectType>(
            mem::ReadU32(objPtr + offsets::objmgr::ObjectType));
        if (type == ObjectType::Unit || type == ObjectType::Player) {
            Unit u(objPtr);
            if (myPos.DistanceTo(u.GetPosition()) <= maxDist)
                units.push_back(u);
        }
        return true;
    });
    return units;
}

std::vector<WowObject> GetAllGameObjects()
{
    std::vector<WowObject> objects;
    objmgr::EnumObjects([&](uintptr_t objPtr) -> bool {
        ObjectType type = static_cast<ObjectType>(
            mem::ReadU32(objPtr + offsets::objmgr::ObjectType));
        if (type == ObjectType::GameObject)
            objects.emplace_back(objPtr);
        return true;
    });
    return objects;
}

bool SelectTarget(GUID guid)
{
    if (guid == GUID_NONE) return false;

    // CGGameUI_Target: void __cdecl(uint64_t guid)
    uintptr_t fn = offsets::fn::CGGameUI_Target;
    uint32_t guidLow  = static_cast<uint32_t>(guid);
    uint32_t guidHigh = static_cast<uint32_t>(guid >> 32);

    __try {
        __asm {
            push guidHigh
            push guidLow
            call fn
            add esp, 8
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }

    return true;
}

} // namespace game
