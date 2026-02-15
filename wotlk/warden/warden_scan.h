#pragma once
#include <cstdint>
#include <cstddef>

namespace warden_scan {

// Reset state — call on MODULE_USE when a new module is about to load
void Reset();

// Scan process memory for Warden module dispatcher, extract check type IDs.
// Returns true if dispatcher was found and IDs were extracted.
bool ScanAndExtractTypeIDs();

// Scan decompressed module binary for dispatcher pattern.
// Primary method — more reliable than memory scan since it searches the exact module.
// Returns true if dispatcher was found and IDs were extracted.
bool ScanModuleBinary(const uint8_t* data, size_t size);

// Try to find the Warden module's runtime base address in process memory.
// Searches for a known stable signature in MEM_PRIVATE executable regions.
// Returns the runtime base address, or 0 if not found.
uintptr_t FindModuleInMemory(const uint8_t* moduleBinary, size_t moduleSize);

// Parse and log the 40-byte Warden module header for diagnostics.
void LogModuleHeader(const uint8_t* data, size_t size);

// Are check type IDs available from a successful scan?
bool HasTypeIDs();

// Is this byte a valid check type ID (from the current module)?
bool IsValidType(uint8_t id);

// Set the number of strings in the current packet (for string index validation).
void SetStringCount(size_t count);

// Deterministic size assignment: iterate through check section, assign sizes
// to new type IDs using structural validation of data bytes.
// Requires HasTypeIDs() to be true. Returns true if all checks parsed ok.
bool AssignTypeSizes(const uint8_t* data, size_t checkStart,
                     size_t checkEnd, uint8_t xorByte);

// Get data size for a check type. Returns -1 if unknown.
int GetDataSize(uint8_t id);

// Are all type sizes known?
bool AllSizesKnown();

// Get human-readable name for a dynamic type ID (based on solved size)
const char* GetTypeName(uint8_t id);

// Get cached runtime address/size of the Warden module (set by FindModuleInMemory).
// Returns 0 if not yet found.
uintptr_t GetModuleRuntimeAddress();
size_t    GetModuleRuntimeSize();

} // namespace warden_scan
