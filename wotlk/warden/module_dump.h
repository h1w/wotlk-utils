#pragma once
#include <cstdint>
#include <cstddef>

namespace module_dump {

// MODULE_USE (0x00): parse hash, RC4 key, expected size — start capture
void OnModuleUse(const uint8_t* data, size_t len);

// MODULE_CACHE (0x01): accumulate chunk data
void OnModuleCache(const uint8_t* data, size_t len);

// MODULE_INITIALIZE (0x03): write assembled module to disk
void OnModuleInitialize(const uint8_t* data, size_t len);

// Reset all state — call on MODULE_USE before starting a new capture
void Reset();

// Try to load a previously cached module from disk.
// hash16 = 16-byte MD5 hash from MODULE_USE packet (offset 1).
// Returns true if g_decompressedModule is now populated.
bool TryLoadFromCache(const uint8_t* hash16);

// Is the decompressed module available?
bool HasDecompressedModule();

// Get pointer to decompressed module data. Returns nullptr if not available.
const uint8_t* GetDecompressedModule(size_t& outLen);

} // namespace module_dump
