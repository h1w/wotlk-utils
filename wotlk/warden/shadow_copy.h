#pragma once
#include <cstdint>
#include <cstddef>

namespace shadow {

// Map Wow.exe from disk, parse PE, find .text section
bool Initialize();

// Unmap + close handles
void Shutdown();

// Copy clean bytes from .text section for the given runtime address.
// Returns false if addr is outside .text section or not initialized.
bool GetCleanBytes(uintptr_t runtimeAddr, uint8_t* out, size_t len);

// Check if address falls within the .text section
bool IsInTextSection(uintptr_t runtimeAddr);

} // namespace shadow
