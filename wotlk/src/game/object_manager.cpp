#include "object_manager.h"
#include "../offsets/offsets.h"
#include "mem.h"

#define NOMINMAX
#include <Windows.h>

namespace game::objmgr {

uintptr_t GetManagerBase()
{
    uintptr_t conn = mem::ReadPointer(offsets::objmgr::CurMgrPointer);
    if (conn == 0) return 0;
    // ObjMgrOffset is a pointer TO the ObjectManager, not an inline struct
    return mem::ReadPointer(conn + offsets::objmgr::ObjMgrOffset);
}

bool IsInGame()
{
    uintptr_t base = GetManagerBase();
    if (base == 0) return false;
    // Verify the first object pointer is readable
    uintptr_t first = mem::ReadPointer(base + offsets::objmgr::FirstObject);
    return first != 0;
}

GUID GetLocalPlayerGUID()
{
    uintptr_t base = GetManagerBase();
    if (base == 0) return GUID_NONE;
    return mem::ReadU64(base + offsets::objmgr::LocalPlayerGUID);
}

uintptr_t GetObjectPtr(GUID guid)
{
    if (guid == GUID_NONE) return 0;

    // ClntObjMgrObjectPtr is __fastcall: ECX=lowGuid, EDX=highGuid
    // Actually it takes the GUID as a struct parameter. We use inline asm.
    uintptr_t result = 0;
    uint32_t guidLow  = static_cast<uint32_t>(guid);
    uint32_t guidHigh = static_cast<uint32_t>(guid >> 32);
    uintptr_t fn = offsets::fn::ClntObjMgrObjectPtr;

    __try {
        __asm {
            push -1          ; filter = -1 (any type)
            push guidHigh
            push guidLow
            call fn
            add esp, 12
            mov result, eax
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        result = 0;
    }

    return result;
}

uintptr_t GetLocalPlayerPtr()
{
    GUID guid = GetLocalPlayerGUID();
    if (guid == GUID_NONE) return 0;

    // Walk linked list directly (pure memory reads, works from any thread)
    uintptr_t base = GetManagerBase();
    if (base == 0) return 0;

    uintptr_t obj = mem::ReadPointer(base + offsets::objmgr::FirstObject);
    int count = 0;
    constexpr int kMaxObjects = 10000;

    while (obj != 0 && count < kMaxObjects) {
        GUID objGuid = mem::ReadU64(obj + offsets::objmgr::ObjectGUID);
        if (objGuid == guid)
            return obj;
        obj = mem::ReadPointer(obj + offsets::objmgr::NextObject);
        ++count;
    }

    return 0;
}

void EnumObjects(const EnumCallback& cb)
{
    uintptr_t base = GetManagerBase();
    if (base == 0) return;

    uintptr_t obj = mem::ReadPointer(base + offsets::objmgr::FirstObject);
    int count = 0;
    constexpr int kMaxObjects = 10000; // safety limit

    while (obj != 0 && count < kMaxObjects) {
        // Validate object type field is within sane range
        int type = static_cast<int>(mem::ReadU32(obj + offsets::objmgr::ObjectType));
        if (type < 0 || type >= static_cast<int>(ObjectType::Max))
            break;

        if (!cb(obj))
            return;

        obj = mem::ReadPointer(obj + offsets::objmgr::NextObject);
        ++count;
    }
}

} // namespace game::objmgr
