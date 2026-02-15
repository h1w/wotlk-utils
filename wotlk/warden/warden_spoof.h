#pragma once
#include <cstdint>
#include <cstddef>
#include <vector>
#include <string>

namespace warden_spoof {

// Check categories for result format selection
enum class CheckCategory : uint8_t {
    TIMING, MEM, PAGE, PROC, MODULE, DRIVER, MPQ, LUA
};

// A single pending check from SMSG CHEAT_CHECKS_REQUEST
struct PendingCheck {
    uint8_t       realType;   // module-specific type ID
    CheckCategory category;   // for result format selection
    uint8_t       readLen;    // MEM/PAGE: byte count to read
    uint32_t      checkAddr;  // MEM/PAGE: target address (0 for others)
    std::string   context;    // human-readable description
};

const char* CheckCategoryToString(CheckCategory cat);

// Initialize/shutdown (critical section)
void Initialize();
void Shutdown();

// Push parsed checks from SMSG (called from hooks.cpp ParseCheatChecksRequest)
void PushPendingChecks(std::vector<PendingCheck>&& checks);

// Pop oldest pending checks (called from hooks.cpp ParseCheatChecksResult)
bool PopPendingChecks(std::vector<PendingCheck>& out);

// Clear queue (called on MODULE_USE — new module = new session)
void ClearPendingChecks();

// Get current queue depth (for logging)
size_t GetQueueDepth();

// Try to spoof CMSG CHEAT_CHECKS_RESULT in-place.
// Called from RC4 hook handler BEFORE encryption.
// Modifies MEM_CHECK/PAGE_CHECK results that target our hook addresses,
// replacing patched bytes with originals from shadow_copy.
// Returns true if buffer was modified.
bool SpoofCmsgIfNeeded(uint8_t* data, size_t len);

} // namespace warden_spoof
