#include "mem.h"

#define NOMINMAX
#include <Windows.h>

namespace game::mem {

// SEH wrappers must be in separate functions (no C++ objects on stack)

static bool __cdecl ReadBytesImpl(uintptr_t addr, void* out, size_t len)
{
    __try {
        const uint8_t* src = reinterpret_cast<const uint8_t*>(addr);
        uint8_t* dst = static_cast<uint8_t*>(out);
        for (size_t i = 0; i < len; ++i)
            dst[i] = src[i];
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static uint32_t __cdecl ReadU32Impl(uintptr_t addr)
{
    __try {
        return *reinterpret_cast<const uint32_t*>(addr);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

static uint64_t __cdecl ReadU64Impl(uintptr_t addr)
{
    __try {
        return *reinterpret_cast<const uint64_t*>(addr);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

static float __cdecl ReadFloatImpl(uintptr_t addr)
{
    __try {
        return *reinterpret_cast<const float*>(addr);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0.f;
    }
}

static uintptr_t __cdecl ReadPointerImpl(uintptr_t addr)
{
    __try {
        return *reinterpret_cast<const uintptr_t*>(addr);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

bool ReadBytes(uintptr_t addr, void* out, size_t len)
{
    if (addr == 0 || out == nullptr || len == 0) return false;
    return ReadBytesImpl(addr, out, len);
}

uint32_t ReadU32(uintptr_t addr)
{
    if (addr == 0) return 0;
    return ReadU32Impl(addr);
}

uint64_t ReadU64(uintptr_t addr)
{
    if (addr == 0) return 0;
    return ReadU64Impl(addr);
}

float ReadFloat(uintptr_t addr)
{
    if (addr == 0) return 0.f;
    return ReadFloatImpl(addr);
}

uintptr_t ReadPointer(uintptr_t addr)
{
    if (addr == 0) return 0;
    return ReadPointerImpl(addr);
}

std::string ReadCString(uintptr_t addr, size_t maxLen)
{
    if (addr == 0) return {};
    char buf[512];
    size_t cap = (maxLen < sizeof(buf)) ? maxLen : sizeof(buf) - 1;
    if (!ReadBytesImpl(addr, buf, cap))
        return {};
    buf[cap] = '\0';
    return std::string(buf);
}

uint32_t ReadDescU32(uintptr_t descriptorBase, int field)
{
    return ReadU32(descriptorBase + static_cast<uintptr_t>(field) * 4);
}

uint64_t ReadDescU64(uintptr_t descriptorBase, int field)
{
    return ReadU64(descriptorBase + static_cast<uintptr_t>(field) * 4);
}

float ReadDescFloat(uintptr_t descriptorBase, int field)
{
    return ReadFloat(descriptorBase + static_cast<uintptr_t>(field) * 4);
}

} // namespace game::mem
