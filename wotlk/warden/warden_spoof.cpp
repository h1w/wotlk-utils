#include "warden_spoof.h"
#include "warden_types.h"
#include "warden_checksum.h"
#include "shadow_copy.h"

#define NOMINMAX
#include <Windows.h>
#include <glog/logging.h>

#include <cstring>
#include <deque>
#include <sstream>
#include <iomanip>

namespace warden_spoof {

// ===========================================================================
// Pending checks FIFO queue
// ===========================================================================

static std::deque<std::vector<PendingCheck>> g_queue;
static CRITICAL_SECTION g_lock;
static bool g_lockInit = false;

// ===========================================================================
// Hook targets: WoW.exe addresses where our inline hooks live.
// MinHook overwrites at least 5 bytes (JMP rel32).  We use 8 as a safe
// upper bound so that any MEM_CHECK whose range overlaps gets spoofed.
// ===========================================================================

static constexpr size_t kHookPatchSize = 8;

static constexpr struct {
    uintptr_t   addr;
    const char* name;
} kHookTargets[] = {
    { 0x00819210, "FrameScript_Execute" },
    { 0x007DA850, "WardenHandler" },
    { 0x00632B50, "SendPacket" },
    { 0x00774EA0, "ARC4::Process" },
};
static constexpr size_t kNumTargets = sizeof(kHookTargets) / sizeof(kHookTargets[0]);

// ===========================================================================
// Helpers
// ===========================================================================

static std::string BytesToHex(const uint8_t* data, size_t len)
{
    std::ostringstream oss;
    for (size_t i = 0; i < len; ++i) {
        if (i > 0) oss << ' ';
        oss << std::hex << std::uppercase << std::setfill('0') << std::setw(2)
            << static_cast<int>(data[i]);
    }
    return oss.str();
}

// Addon-detection API patterns (case-sensitive substring match in eval code)
static const char* kAddonPatterns[] = {
    "GetAddOnInfo",
    "IsAddOnLoaded",
    "GetAddOnMetadata",
    "GetAddOnEnableState",
    "GetAddOnDependencies",
    "GetAddOnOptionalDependencies",
};

static bool ShouldSpoofLuaResult(const std::string& evalCode)
{
    if (evalCode.empty())
        return false;
    for (const auto& pattern : kAddonPatterns) {
        if (evalCode.find(pattern) != std::string::npos)
            return true;
    }
    return false;
}

// Check if [addr, addr+len) overlaps with any hook target.
// Returns index into kHookTargets or -1.
static int FindOverlappingHook(uint32_t addr, uint8_t len)
{
    for (size_t i = 0; i < kNumTargets; ++i) {
        uintptr_t hookStart = kHookTargets[i].addr;
        uintptr_t hookEnd   = hookStart + kHookPatchSize;
        uintptr_t chkStart  = addr;
        uintptr_t chkEnd    = addr + static_cast<uintptr_t>(len);
        if (chkStart < hookEnd && hookStart < chkEnd)
            return static_cast<int>(i);
    }
    return -1;
}

const char* CheckCategoryToString(CheckCategory cat)
{
    switch (cat) {
    case CheckCategory::TIMING: return "TIMING";
    case CheckCategory::MEM:    return "MEM";
    case CheckCategory::PAGE:   return "PAGE";
    case CheckCategory::PROC:   return "PROC";
    case CheckCategory::MODULE: return "MODULE";
    case CheckCategory::DRIVER: return "DRIVER";
    case CheckCategory::MPQ:    return "MPQ";
    case CheckCategory::LUA:    return "LUA";
    default:                    return "UNKNOWN";
    }
}

// ===========================================================================
// Public API: queue management
// ===========================================================================

void Initialize()
{
    if (!g_lockInit) {
        InitializeCriticalSection(&g_lock);
        g_lockInit = true;
    }
}

void Shutdown()
{
    if (g_lockInit) {
        DeleteCriticalSection(&g_lock);
        g_lockInit = false;
    }
}

void PushPendingChecks(std::vector<PendingCheck>&& checks)
{
    if (!g_lockInit) return;
    EnterCriticalSection(&g_lock);
    g_queue.push_back(std::move(checks));
    LeaveCriticalSection(&g_lock);
}

bool PopPendingChecks(std::vector<PendingCheck>& out)
{
    if (!g_lockInit) return false;
    EnterCriticalSection(&g_lock);
    if (g_queue.empty()) {
        LeaveCriticalSection(&g_lock);
        return false;
    }
    out = std::move(g_queue.front());
    g_queue.pop_front();
    LeaveCriticalSection(&g_lock);
    return true;
}

void ClearPendingChecks()
{
    if (!g_lockInit) return;
    EnterCriticalSection(&g_lock);
    g_queue.clear();
    LeaveCriticalSection(&g_lock);
}

size_t GetQueueDepth()
{
    if (!g_lockInit) return 0;
    EnterCriticalSection(&g_lock);
    size_t depth = g_queue.size();
    LeaveCriticalSection(&g_lock);
    return depth;
}

// ===========================================================================
// HASH_REQUEST seed storage
// ===========================================================================

static uint8_t g_hashSeed[16] = {};
static bool    g_hashSeedValid = false;

void StoreHashSeed(const uint8_t* seed, size_t len)
{
    if (!seed || len < 16)
        return;
    std::memcpy(g_hashSeed, seed, 16);
    g_hashSeedValid = true;
    LOG(INFO) << "[SPOOF] Stored HASH_REQUEST seed: " << BytesToHex(g_hashSeed, 16);
}

const uint8_t* GetStoredHashSeed()
{
    return g_hashSeedValid ? g_hashSeed : nullptr;
}

bool SpoofHashResultIfNeeded(uint8_t* data, size_t len)
{
    // Currently a no-op stub.
    // The module computes the correct hash because our hooks don't modify
    // its code before HASH_RESULT is sent. Infrastructure for future use.
    if (!data || len != 21 || data[0] != WARDEN_CMSG_HASH_RESULT)
        return false;

    LOG(INFO) << "[SPOOF] HASH_RESULT seen (21 bytes), SHA1=["
              << BytesToHex(data + 1, 20) << "] — not modified (stub)";
    return false;
}

// ===========================================================================
// Core: spoof CMSG CHEAT_CHECKS_RESULT (copy-based rebuild)
//
// CMSG format: [0x02][resultLen:2 LE][checksum:4][results:N]
//
// Walk results using pending checks (front of queue, peek only).
// Copies each result from old buffer to new buffer, modifying as needed:
//   - MEM_CHECK targeting hook address: replace data with shadow_copy originals
//   - PAGE_CHECK targeting hook address: force 0xE9 (pass)
//   - LUA_EVAL with addon-detection eval code + non-empty result: replace with empty string
// Copy-based approach handles variable-length LUA results correctly
// (in-place would break offsets when result shrinks).
// If modified: update resultLen, copy back, zero trailing, recompute checksum.
// ===========================================================================

bool SpoofCmsgIfNeeded(uint8_t* data, size_t len)
{
    // Validate: must be CHEAT_CHECKS_RESULT, at least 7 bytes
    if (!data || len < 7 || data[0] != WARDEN_CMSG_CHEAT_CHECKS_RESULT)
        return false;

    uint16_t resultLen = static_cast<uint16_t>(data[1]) |
                         (static_cast<uint16_t>(data[2]) << 8);
    if (len < static_cast<size_t>(7) + resultLen || resultLen == 0)
        return false;

    // Peek at front of pending checks queue (don't pop — ParseCheatChecksResult will pop later)
    if (!g_lockInit) return false;
    EnterCriticalSection(&g_lock);
    if (g_queue.empty()) {
        LeaveCriticalSection(&g_lock);
        return false;
    }
    // Copy the front entry while holding lock
    std::vector<PendingCheck> checks = g_queue.front();
    LeaveCriticalSection(&g_lock);

    if (checks.empty())
        return false;

    // Copy-based walk: read from oldResults, write to newResults
    const uint8_t* oldResults = data + 7;
    uint8_t newResults[4096];
    size_t oldPos = 0, newPos = 0;
    bool modified = false;
    int spoofCount = 0;

    for (size_t i = 0; i < checks.size(); ++i) {
        const auto& chk = checks[i];

        if (oldPos >= resultLen)
            break;

        uint8_t resultByte = oldResults[oldPos];

        switch (chk.category) {
        case CheckCategory::TIMING:
            // 5 bytes always: [result:1][ticks:4]
            if (oldPos + 5 > resultLen) goto done;
            std::memcpy(newResults + newPos, oldResults + oldPos, 5);
            oldPos += 5; newPos += 5;
            break;

        case CheckCategory::MEM:
            if (resultByte != 0x00) {
                // Fail — 1 byte
                newResults[newPos++] = oldResults[oldPos++];
            } else {
                // Success: [0x00][data:readLen]
                size_t totalSize = 1 + chk.readLen;
                if (oldPos + totalSize > resultLen) goto done;

                std::memcpy(newResults + newPos, oldResults + oldPos, totalSize);

                // Check if this MEM_CHECK targets one of our hooks
                if (chk.checkAddr != 0 && chk.readLen > 0) {
                    int hookIdx = FindOverlappingHook(chk.checkAddr, chk.readLen);
                    if (hookIdx >= 0) {
                        uint8_t clean[256];
                        if (chk.readLen <= sizeof(clean) &&
                            shadow::GetCleanBytes(chk.checkAddr, clean, chk.readLen))
                        {
                            std::memcpy(newResults + newPos + 1, clean, chk.readLen);
                            modified = true;
                            spoofCount++;

                            LOG(INFO) << "[SPOOF] MEM_CHECK #" << std::dec << (i + 1)
                                      << " addr=0x" << std::hex << std::setfill('0')
                                      << std::setw(8) << chk.checkAddr
                                      << " len=" << std::dec << (int)chk.readLen
                                      << " target=" << kHookTargets[hookIdx].name
                                      << " replaced with clean bytes=["
                                      << BytesToHex(clean, chk.readLen) << "]";
                        }
                    }
                }
                oldPos += totalSize; newPos += totalSize;
            }
            break;

        case CheckCategory::PAGE:
            // 1 byte result: 0xE9 = pass, anything else = fail
            newResults[newPos] = resultByte;
            if (chk.checkAddr != 0 && chk.readLen > 0) {
                int hookIdx = FindOverlappingHook(chk.checkAddr, chk.readLen);
                if (hookIdx >= 0 && resultByte != 0xE9) {
                    LOG(INFO) << "[SPOOF] PAGE_CHECK #" << std::dec << (i + 1)
                              << " addr=0x" << std::hex << std::setfill('0')
                              << std::setw(8) << chk.checkAddr
                              << " target=" << kHookTargets[hookIdx].name
                              << " result=0x" << std::setw(2) << (int)resultByte
                              << " -> 0xE9 (forced pass)";
                    newResults[newPos] = 0xE9;
                    modified = true;
                    spoofCount++;
                }
            }
            oldPos += 1; newPos += 1;
            break;

        case CheckCategory::MODULE:
            // 1-byte result: 0xE9 = not found (pass). Force pass as backup to PEB unlinking.
            if (oldResults[oldPos] != 0xE9) {
                LOG(INFO) << "[SPOOF] MODULE_CHECK #" << std::dec << (i + 1)
                          << " result=0x" << std::hex << std::setfill('0')
                          << std::setw(2) << (int)oldResults[oldPos]
                          << " -> 0xE9 (forced pass)";
                newResults[newPos++] = 0xE9;
                modified = true;
                spoofCount++;
            } else {
                newResults[newPos++] = oldResults[oldPos];
            }
            oldPos++;
            break;

        case CheckCategory::PROC:
        case CheckCategory::DRIVER:
            // Fixed 1-byte result
            newResults[newPos++] = oldResults[oldPos++];
            break;

        case CheckCategory::MPQ:
            if (resultByte != 0x00) {
                newResults[newPos++] = oldResults[oldPos++];
            } else {
                // [0x00][SHA1:20]
                if (oldPos + 21 > resultLen) goto done;
                std::memcpy(newResults + newPos, oldResults + oldPos, 21);
                oldPos += 21; newPos += 21;
            }
            break;

        case CheckCategory::LUA:
            if (resultByte != 0x00) {
                // Fail — 1 byte
                newResults[newPos++] = oldResults[oldPos++];
            } else {
                // Success: [0x00][strlen:1][string:N]
                if (oldPos + 2 > resultLen) goto done;
                uint8_t strLen = oldResults[oldPos + 1];
                if (oldPos + 2 + strLen > resultLen) goto done;

                if (strLen > 0 && ShouldSpoofLuaResult(chk.context)) {
                    // Addon-detection: replace non-empty string with empty
                    std::string origStr(
                        reinterpret_cast<const char*>(oldResults + oldPos + 2), strLen);
                    newResults[newPos++] = 0x00;  // result = success
                    newResults[newPos++] = 0x00;  // strlen = 0
                    modified = true;
                    spoofCount++;

                    LOG(INFO) << "[SPOOF] LUA_EVAL #" << std::dec << (i + 1)
                              << " eval=\"" << chk.context << "\""
                              << " result=\"" << origStr << "\" -> empty";
                } else {
                    // Not addon-related OR already empty — copy as-is
                    std::memcpy(newResults + newPos, oldResults + oldPos, 2 + strLen);
                    newPos += 2 + strLen;
                }
                oldPos += 2 + strLen;
            }
            break;
        }
    }

done:
    // Copy any remaining result bytes we didn't parse (partial pending checks
    // list means we only know about the first N checks, but the client produced
    // results for ALL checks — pass the rest through unchanged).
    if (oldPos < resultLen) {
        size_t remaining = resultLen - oldPos;
        if (newPos + remaining <= sizeof(newResults)) {
            std::memcpy(newResults + newPos, oldResults + oldPos, remaining);
            newPos += remaining;
        }
    }

    if (!modified)
        return false;

    // Update resultLen in header
    uint16_t newResultLen = static_cast<uint16_t>(newPos);
    std::memcpy(data + 1, &newResultLen, 2);

    // Copy new results back
    std::memcpy(data + 7, newResults, newPos);

    // Zero trailing bytes (cleanliness — server won't read past newResultLen)
    if (newPos < resultLen)
        std::memset(data + 7 + newPos, 0, resultLen - newPos);

    // Recompute checksum over new results
    uint32_t newChecksum = warden_checksum::BuildChecksum(data + 7, newResultLen);
    std::memcpy(data + 3, &newChecksum, 4);

    LOG(INFO) << "[SPOOF] Spoofed " << std::dec << spoofCount
              << " check(s), resultLen " << resultLen << " -> " << newResultLen
              << ", new checksum=0x" << std::hex << std::setfill('0')
              << std::setw(8) << newChecksum;

    return true;
}

} // namespace warden_spoof
