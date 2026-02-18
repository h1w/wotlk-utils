#include "game_object.h"
#include "../offsets/offsets.h"
#include "mem.h"

#define NOMINMAX
#include <Windows.h>

namespace game {

// SEH-safe vtable call helpers — no C++ objects on stack

static bool __cdecl CallVTGetPosition(uintptr_t objPtr, uintptr_t fn, Vec3* outPos)
{
    __try {
        __asm {
            mov eax, outPos
            push eax
            mov ecx, objPtr
            call fn
        }
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static float __cdecl CallVTGetFacing(uintptr_t objPtr, uintptr_t fn)
{
    float result = 0.f;
    __try {
        __asm {
            mov ecx, objPtr
            call fn
            fstp result
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        result = 0.f;
    }
    return result;
}

static const char* __cdecl CallVTGetName(uintptr_t objPtr, uintptr_t fn)
{
    __try {
        const char* namePtr = nullptr;
        __asm {
            mov ecx, objPtr
            call fn
            mov namePtr, eax
        }
        return namePtr;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

// ---------------------------------------------------------------------------

uintptr_t WowObject::GetDescriptorBase() const
{
    return mem::ReadPointer(m_ptr + offsets::objmgr::Descriptor);
}

uintptr_t WowObject::GetVTable() const
{
    return mem::ReadPointer(m_ptr); // vtable is at offset 0
}

GUID WowObject::GetGUID() const
{
    return mem::ReadU64(m_ptr + offsets::objmgr::ObjectGUID);
}

ObjectType WowObject::GetType() const
{
    int t = static_cast<int>(mem::ReadU32(m_ptr + offsets::objmgr::ObjectType));
    if (t < 0 || t >= static_cast<int>(ObjectType::Max))
        return ObjectType::Object;
    return static_cast<ObjectType>(t);
}

uint32_t WowObject::GetEntry() const
{
    uintptr_t desc = GetDescriptorBase();
    if (desc == 0) return 0;
    return mem::ReadDescU32(desc, offsets::fields::OBJECT_ENTRY);
}

float WowObject::GetScale() const
{
    uintptr_t desc = GetDescriptorBase();
    if (desc == 0) return 1.f;
    return mem::ReadDescFloat(desc, offsets::fields::OBJECT_SCALE);
}

uint32_t WowObject::GetDescU32(int field) const
{
    uintptr_t desc = GetDescriptorBase();
    if (desc == 0) return 0;
    return mem::ReadDescU32(desc, field);
}

uint64_t WowObject::GetDescU64(int field) const
{
    uintptr_t desc = GetDescriptorBase();
    if (desc == 0) return 0;
    return mem::ReadDescU64(desc, field);
}

float WowObject::GetDescFloat(int field) const
{
    uintptr_t desc = GetDescriptorBase();
    if (desc == 0) return 0.f;
    return mem::ReadDescFloat(desc, field);
}

Vec3 WowObject::GetPosition() const
{
    Vec3 pos;
    uintptr_t vt = GetVTable();
    if (vt == 0) return pos;

    uintptr_t fn = mem::ReadPointer(vt + offsets::vtable::GetPosition * 4);
    if (fn == 0) return pos;

    if (!CallVTGetPosition(m_ptr, fn, &pos))
        pos = {};
    return pos;
}

float WowObject::GetFacing() const
{
    uintptr_t vt = GetVTable();
    if (vt == 0) return 0.f;

    uintptr_t fn = mem::ReadPointer(vt + offsets::vtable::GetFacing * 4);
    if (fn == 0) return 0.f;

    return CallVTGetFacing(m_ptr, fn);
}

float WowObject::DistanceTo(const WowObject& other) const
{
    return GetPosition().DistanceTo(other.GetPosition());
}

std::string WowObject::GetName() const
{
    uintptr_t vt = GetVTable();
    if (vt == 0) return {};

    uintptr_t fn = mem::ReadPointer(vt + offsets::vtable::GetName * 4);
    if (fn == 0) return {};

    const char* namePtr = CallVTGetName(m_ptr, fn);
    if (!namePtr) return {};
    return mem::ReadCString(reinterpret_cast<uintptr_t>(namePtr));
}

} // namespace game
