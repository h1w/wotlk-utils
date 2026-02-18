#include "world.h"
#include "../offsets/offsets.h"
#include "mem.h"
#include "object_manager.h"

#define NOMINMAX
#include <Windows.h>

namespace game::world {

std::string GetZoneText()
{
    uintptr_t ptr = mem::ReadPointer(offsets::globals::ZoneText);
    if (ptr == 0) return {};
    return mem::ReadCString(ptr);
}

std::string GetSubZoneText()
{
    uintptr_t ptr = mem::ReadPointer(offsets::globals::SubZoneText);
    if (ptr == 0) return {};
    return mem::ReadCString(ptr);
}

uint32_t GetZoneId()
{
    return mem::ReadU32(offsets::globals::ZoneId);
}

uint32_t GetMapId()
{
    return mem::ReadU32(offsets::globals::MapId);
}

std::string GetRealmName()
{
    return mem::ReadCString(offsets::globals::RealmName);
}

bool IsInGame()
{
    return objmgr::IsInGame();
}

bool IsLoading()
{
    return mem::ReadU32(offsets::globals::IsLoadingOrConnecting) != 0;
}

bool HasLineOfSight(const Vec3& start, const Vec3& end)
{
    // TraceLine: int __cdecl(Vec3* start, Vec3* end, Vec3* hitPoint, float* dist, uint32_t flags)
    // Returns 0 if no collision (clear LOS), non-zero if blocked.
    using TraceLineFn = int(__cdecl*)(const Vec3*, const Vec3*, Vec3*, float*, uint32_t);
    auto fn = reinterpret_cast<TraceLineFn>(offsets::fn::TraceLine);

    Vec3 hitPoint;
    float hitDist = 1.0f;
    int result = 0;

    __try {
        // flags: 0x100111 = collision with terrain + wmo + m2
        result = fn(&start, &end, &hitPoint, &hitDist, 0x100111);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }

    return result == 0;
}

Vec3 GetCameraPosition()
{
    Vec3 pos;
    // CameraPointer -> camera struct, position at offset +0x08
    uintptr_t camPtr = mem::ReadPointer(offsets::globals::CameraPointer);
    if (camPtr == 0) return pos;
    uintptr_t cam = mem::ReadPointer(camPtr + 0x7E20); // camera object offset
    if (cam == 0) return pos;
    mem::ReadBytes(cam + 0x08, &pos, sizeof(Vec3));
    return pos;
}

} // namespace game::world
