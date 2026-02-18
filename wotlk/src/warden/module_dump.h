#pragma once
#include <cstdint>
#include <cstddef>
#include <string>

namespace module_dump {

// Set the base directory for warden dump files (e.g. "Z:\Games\wow\wotlk\warden_dumps").
// Must be called before any capture.  Creates the directory if it doesn't exist.
void SetOutputDir(const std::string& dir);

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

// Save a Warden module found in process memory (runtime image) to disk.
// Also populates g_decompressedModule for subsequent use (type scanning, etc.).
// base = runtime base address (VirtualAlloc'd block), size = region size.
void SaveFromMemory(uintptr_t base, size_t size);

} // namespace module_dump
