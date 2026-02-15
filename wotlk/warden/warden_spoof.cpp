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
// Core: spoof CMSG CHEAT_CHECKS_RESULT in-place
//
// CMSG format: [0x02][resultLen:2 LE][checksum:4][results:N]
//
// Walk results using pending checks (front of queue, peek only).
// For each MEM_CHECK result=0x00 (success) targeting a hook address:
//   replace the data bytes with original bytes from shadow_copy.
// For each PAGE_CHECK targeting a hook address with result != 0xE9:
//   replace result byte with 0xE9 (pass).
// If any modification was made, recompute checksum.
// ===========================================================================

bool SpoofCmsgIfNeeded(uint8_t* data, size_t len)
{
    // Validate: must be CHEAT_CHECKS_RESULT, at least 7 bytes
    if (!data || len < 7 || data[0] != WARDEN_CMSG_CHEAT_CHECKS_RESULT)
        return false;

    uint16_t resultLen = static_cast<uint16_t>(data[1]) |
                         (static_cast<uint16_t>(data[2]) << 8);
    if (len != static_cast<size_t>(7) + resultLen || resultLen == 0)
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

    // Walk results section in check order
    uint8_t* results = data + 7;
    size_t pos = 0;
    bool modified = false;
    int spoofCount = 0;

    for (size_t i = 0; i < checks.size(); ++i) {
        const auto& chk = checks[i];

        if (pos >= resultLen)
            break;

        uint8_t resultByte = results[pos];

        switch (chk.category) {
        case CheckCategory::TIMING:
            // 5 bytes always: [result:1][ticks:4]
            if (pos + 5 > resultLen) { pos = resultLen; continue; }
            pos += 5;
            break;

        case CheckCategory::MEM:
            if (resultByte != 0x00) {
                // Fail — 1 byte
                pos += 1;
            } else {
                // Success: [0x00][data:readLen]
                size_t totalSize = 1 + chk.readLen;
                if (pos + totalSize > resultLen) { pos = resultLen; continue; }

                // Check if this MEM_CHECK targets one of our hooks
                if (chk.checkAddr != 0 && chk.readLen > 0) {
                    int hookIdx = FindOverlappingHook(chk.checkAddr, chk.readLen);
                    if (hookIdx >= 0) {
                        // Read original bytes from shadow_copy
                        uint8_t clean[256];
                        if (chk.readLen <= sizeof(clean) &&
                            shadow::GetCleanBytes(chk.checkAddr, clean, chk.readLen))
                        {
                            std::memcpy(results + pos + 1, clean, chk.readLen);
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
                pos += totalSize;
            }
            break;

        case CheckCategory::PAGE:
            // 1 byte result: 0xE9 = pass, anything else = fail
            if (chk.checkAddr != 0 && chk.readLen > 0) {
                int hookIdx = FindOverlappingHook(chk.checkAddr, chk.readLen);
                if (hookIdx >= 0 && resultByte != 0xE9) {
                    LOG(INFO) << "[SPOOF] PAGE_CHECK #" << std::dec << (i + 1)
                              << " addr=0x" << std::hex << std::setfill('0')
                              << std::setw(8) << chk.checkAddr
                              << " target=" << kHookTargets[hookIdx].name
                              << " result=0x" << std::setw(2) << (int)resultByte
                              << " -> 0xE9 (forced pass)";
                    results[pos] = 0xE9;
                    modified = true;
                    spoofCount++;
                }
            }
            pos += 1;
            break;

        case CheckCategory::PROC:
        case CheckCategory::MODULE:
        case CheckCategory::DRIVER:
            // Fixed 1-byte result
            pos += 1;
            break;

        case CheckCategory::MPQ:
            if (resultByte != 0x00) {
                pos += 1;
            } else {
                // [0x00][SHA1:20]
                if (pos + 21 > resultLen) { pos = resultLen; continue; }
                pos += 21;
            }
            break;

        case CheckCategory::LUA:
            if (resultByte != 0x00) {
                pos += 1;
            } else {
                // [0x00][strlen:1][string:N]
                if (pos + 2 > resultLen) { pos = resultLen; continue; }
                uint8_t strLen = results[pos + 1];
                if (pos + 2 + strLen > resultLen) { pos = resultLen; continue; }
                pos += 2 + strLen;
            }
            break;
        }
    }

    if (!modified)
        return false;

    // Recompute checksum over modified results
    uint32_t newChecksum = warden_checksum::BuildChecksum(results, resultLen);
    std::memcpy(data + 3, &newChecksum, 4);

    LOG(INFO) << "[SPOOF] Spoofed " << std::dec << spoofCount
              << " check(s), new checksum=0x" << std::hex << std::setfill('0')
              << std::setw(8) << newChecksum;

    return true;
}

} // namespace warden_spoof
