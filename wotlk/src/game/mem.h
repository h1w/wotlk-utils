#pragma once
// =============================================================================
// SEH-safe memory reading utilities for the game SDK.
// All read functions return false / 0 / "" on access violation.
// =============================================================================

#include <cstdint>
#include <cstddef>
#include <string>

namespace game::mem {

bool     ReadBytes(uintptr_t addr, void* out, size_t len);
uint32_t ReadU32(uintptr_t addr);
uint64_t ReadU64(uintptr_t addr);
float    ReadFloat(uintptr_t addr);
uintptr_t ReadPointer(uintptr_t addr);

// Read a null-terminated C string (up to maxLen chars). Returns "" on failure.
std::string ReadCString(uintptr_t addr, size_t maxLen = 256);

// Read a descriptor field (descriptor base + fieldIndex * 4)
uint32_t ReadDescU32(uintptr_t descriptorBase, int field);
uint64_t ReadDescU64(uintptr_t descriptorBase, int field);
float    ReadDescFloat(uintptr_t descriptorBase, int field);

} // namespace game::mem
