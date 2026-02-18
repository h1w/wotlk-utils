#include "warden_rc4.h"
#include "warden_types.h"
#include "../offsets/offsets.h"

#define NOMINMAX
#include <Windows.h>
#include <glog/logging.h>

#include <cstring>
#include <vector>
#include <sstream>
#include <iomanip>

// ---------------------------------------------------------------------------
// RC4 state: [i (1 byte)][j (1 byte)][S[256]]
// Two possible layouts in memory:
//   layout 0: [i][j][S[0]..S[255]]   — i,j before S-box
//   layout 1: [S[0]..S[255]][i][j]   — i,j after S-box
// ---------------------------------------------------------------------------

namespace {

struct RC4State {
    uint8_t i;
    uint8_t j;
    uint8_t S[256];
};

struct RC4Candidate {
    uintptr_t addr;          // Address of S[0] in target memory
    int       layout;        // 0 = [i][j][S], 1 = [S][i][j]
    int       elemSize;      // 1 = uint8_t S[256], 4 = uint32_t S[256]
    RC4State  snapshot;      // Cloned state from last CloneAllStates()
    RC4State  running;       // Running clone (advanced after each decrypt)
    bool      isEncryptState;
    bool      cloneValid;    // snapshot was successfully read
};

static std::vector<RC4Candidate> g_candidates;
static int  g_encryptIdx  = -1;  // Index into g_candidates, -1 = not yet identified
static bool g_scanDone    = false;
static bool g_clonesReady = false;  // True if valid clones are pending for identification

// Thread safety: CloneAllStates runs on main thread (WardenPreHandler),
// DecryptCmsg/AdvanceEncryptState may run from Warden module thread (SendPacket).
static CRITICAL_SECTION g_lock;
static bool g_lockInitialized = false;

static void EnsureLockInitialized()
{
    if (!g_lockInitialized) {
        InitializeCriticalSection(&g_lock);
        g_lockInitialized = true;
    }
}

// Max region size to scan (64 MB — large heap regions may contain S-boxes)
static constexpr size_t kMaxRegionSize = 64 * 1024 * 1024;

// ---------------------------------------------------------------------------
// SEH-safe memory read (manual loop — no CRT intrinsics for SEH safety)
// ---------------------------------------------------------------------------
static bool __cdecl SafeReadBytes(const void* src, void* dst, size_t count)
{
    __try {
        const uint8_t* s = static_cast<const uint8_t*>(src);
        uint8_t* d = static_cast<uint8_t*>(dst);
        for (size_t i = 0; i < count; ++i)
            d[i] = s[i];
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// ---------------------------------------------------------------------------
// RC4 PRGA: generate one keystream byte and advance state
// ---------------------------------------------------------------------------
static uint8_t RC4NextByte(RC4State& st)
{
    st.i++;
    st.j += st.S[st.i];
    uint8_t tmp = st.S[st.i];
    st.S[st.i] = st.S[st.j];
    st.S[st.j] = tmp;
    return st.S[(uint8_t)(st.S[st.i] + st.S[st.j])];
}

// ---------------------------------------------------------------------------
// Check if 256 bytes form a permutation of 0..255
// ---------------------------------------------------------------------------
static bool IsPermutation256(const uint8_t* data)
{
    uint8_t seen[256] = {};
    for (int i = 0; i < 256; ++i) {
        if (seen[data[i]])
            return false;
        seen[data[i]] = 1;
    }
    return true;
}

// Check if 256 uint32_t DWORDs form a permutation of 0..255
static bool IsPermutation256_32(const uint32_t* data)
{
    uint8_t seen[256] = {};
    for (int i = 0; i < 256; ++i) {
        if (data[i] > 255)
            return false;
        uint8_t val = static_cast<uint8_t>(data[i]);
        if (seen[val])
            return false;
        seen[val] = 1;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Check if a Warden client opcode is valid
// ---------------------------------------------------------------------------
static bool IsValidClientOpcode(uint8_t op)
{
    switch (op) {
    case WARDEN_CMSG_MODULE_MISSING:
    case WARDEN_CMSG_MODULE_OK:
    case WARDEN_CMSG_CHEAT_CHECKS_RESULT:
    case WARDEN_CMSG_MEM_CHECKS_RESULT:
    case WARDEN_CMSG_HASH_RESULT:
        return true;
    default:
        return false;
    }
}

// ---------------------------------------------------------------------------
// Structural validation of a fully-decrypted CMSG_WARDEN_DATA payload.
// Checks opcode + length constraints to reject false positives.
//
// CMSG formats:
//   0x00 MODULE_MISSING:      [op] — exactly 1 byte
//   0x01 MODULE_OK:           [op] — exactly 1 byte
//   0x02 CHEAT_CHECKS_RESULT: [op][uint16_LE resultLen][result[resultLen]][uint32 checksum]
//                              total = 7 + resultLen
//   0x04 MEM_CHECKS_RESULT:   [op][result data...] — at least 1 byte
//   0x05 HASH_RESULT:         [op][20-byte SHA1] — exactly 21 bytes
// ---------------------------------------------------------------------------
static bool ValidateDecryptedCmsg(const uint8_t* plaintext, size_t len)
{
    if (len == 0) return false;

    switch (plaintext[0]) {
    case WARDEN_CMSG_MODULE_MISSING:      // 0x00
    case WARDEN_CMSG_MODULE_OK:           // 0x01
        return len == 1;

    case WARDEN_CMSG_CHEAT_CHECKS_RESULT: // 0x02
        if (len < 7) return false;
        {
            uint16_t resultLen = static_cast<uint16_t>(plaintext[1]) |
                                 (static_cast<uint16_t>(plaintext[2]) << 8);
            return len == static_cast<size_t>(7) + resultLen;
        }

    case WARDEN_CMSG_MEM_CHECKS_RESULT:   // 0x04
        return len >= 1;

    case WARDEN_CMSG_HASH_RESULT:         // 0x05
        return len == 21;

    default:
        return false;
    }
}

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

} // anonymous namespace

// ===========================================================================
// Public API
// ===========================================================================

namespace warden_rc4 {

void Reset()
{
    EnsureLockInitialized();
    EnterCriticalSection(&g_lock);

    g_candidates.clear();
    g_encryptIdx  = -1;
    g_scanDone    = false;
    g_clonesReady = false;

    LeaveCriticalSection(&g_lock);
    LOG(INFO) << "[RC4] State reset (new module loading)";
}

// ---------------------------------------------------------------------------
// Helper: enumerate readable memory regions (MEM_PRIVATE + MEM_IMAGE),
// calling `callback(base, buf, regionSize)` for each.
// ---------------------------------------------------------------------------
template <typename Fn>
static void EnumReadableRegions(Fn callback, size_t minSize,
                                size_t& regionsScanned, size_t& bytesScanned)
{
    MEMORY_BASIC_INFORMATION mbi;
    uintptr_t addr = 0x10000; // Skip null page

    while (VirtualQuery(reinterpret_cast<LPCVOID>(addr), &mbi, sizeof(mbi)) == sizeof(mbi)) {
        uintptr_t regionEnd = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
        if (regionEnd <= addr) break;

        bool readable = (mbi.State == MEM_COMMIT) &&
                        (mbi.Type == MEM_PRIVATE || mbi.Type == MEM_IMAGE) &&
                        (mbi.Protect & (PAGE_READWRITE | PAGE_READONLY |
                                        PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE));
        if (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS))
            readable = false;

        uintptr_t base = reinterpret_cast<uintptr_t>(mbi.BaseAddress);

        if (!readable || mbi.RegionSize > kMaxRegionSize || mbi.RegionSize < minSize) {
            addr = regionEnd;
            continue;
        }

        // Exclude WoW .text section
        if (base >= offsets::TextStart && base < offsets::TextEnd) {
            addr = regionEnd;
            continue;
        }

        size_t regionSize = mbi.RegionSize;
        std::vector<uint8_t> buf(regionSize);
        if (!SafeReadBytes(reinterpret_cast<const void*>(base), buf.data(), regionSize)) {
            addr = regionEnd;
            continue;
        }

        regionsScanned++;
        bytesScanned += regionSize;

        callback(base, buf.data(), regionSize);

        addr = regionEnd;
    }
}

// ---------------------------------------------------------------------------
// Scan a region buffer for uint8_t S[256] permutations (256 bytes)
// ---------------------------------------------------------------------------
static void ScanRegion8(uintptr_t base, const uint8_t* buf, size_t regionSize,
                        std::vector<RC4Candidate>& found)
{
    static constexpr size_t kWindowSize = 256;
    if (regionSize < kWindowSize)
        return;

    uint16_t freq[256] = {};
    int uniqueCount = 0;

    for (size_t i = 0; i < kWindowSize; ++i) {
        if (++freq[buf[i]] == 1)
            uniqueCount++;
    }

    size_t pos = 0;
    while (pos + kWindowSize <= regionSize) {
        if (pos > 0) {
            uint8_t oldByte = buf[pos - 1];
            if (--freq[oldByte] == 0)
                uniqueCount--;
            uint8_t newByte = buf[pos + kWindowSize - 1];
            if (++freq[newByte] == 1)
                uniqueCount++;
        }

        if (uniqueCount == 256) {
            uintptr_t sboxAddr = base + pos;

            RC4Candidate cand;
            cand.addr = sboxAddr;
            cand.elemSize = 1;
            cand.isEncryptState = false;
            cand.cloneValid = false;

            if (pos >= 2) {
                cand.layout = 0;
                found.push_back(cand);
                LOG(INFO) << "[RC4_SCAN] Found uint8_t S-box #" << found.size()
                          << " at 0x" << std::hex << std::setfill('0')
                          << std::setw(8) << sboxAddr
                          << " (layout=[i8][j8][S8])";
            }

            if (pos + kWindowSize + 2 <= regionSize) {
                cand.layout = 1;
                found.push_back(cand);
                LOG(INFO) << "[RC4_SCAN] Found uint8_t S-box #" << found.size()
                          << " at 0x" << std::hex << std::setfill('0')
                          << std::setw(8) << sboxAddr
                          << " (layout=[S8][i8][j8])";
            }

            size_t newPos = pos + kWindowSize;
            if (newPos + kWindowSize > regionSize)
                break;

            memset(freq, 0, sizeof(freq));
            uniqueCount = 0;
            for (size_t i = 0; i < kWindowSize; ++i) {
                if (++freq[buf[newPos + i]] == 1)
                    uniqueCount++;
            }
            pos = newPos;
            continue;
        }

        pos++;
    }
}

// ---------------------------------------------------------------------------
// Scan a region buffer for uint32_t S[256] permutations (1024 bytes)
// Uses O(N) sliding window over DWORDs with value 0-255.
// ---------------------------------------------------------------------------
static void ScanRegion32(uintptr_t base, const uint8_t* buf, size_t regionSize,
                         std::vector<RC4Candidate>& found)
{
    static constexpr size_t kSboxDwords = 256;
    static constexpr size_t kSboxBytes  = kSboxDwords * 4; // 1024

    if (regionSize < kSboxBytes)
        return;

    size_t numDwords = regionSize / 4;
    if (numDwords < kSboxDwords)
        return;

    // x86-only: unaligned uint32_t reads are fine on this platform (MSVC/x86)
    const uint32_t* dw = reinterpret_cast<const uint32_t*>(buf);

    uint16_t freq[256] = {};
    int uniqueCount = 0;
    size_t windowStart = 0;

    for (size_t i = 0; i < numDwords; ++i) {
        uint32_t val = dw[i];

        if (val > 255) {
            // Invalid DWORD — reset window entirely
            memset(freq, 0, sizeof(freq));
            uniqueCount = 0;
            windowStart = i + 1;
            continue;
        }

        uint8_t v = static_cast<uint8_t>(val);
        if (++freq[v] == 1)
            uniqueCount++;

        size_t windowLen = i - windowStart + 1;

        if (windowLen > kSboxDwords) {
            // Slide: remove oldest DWORD
            uint8_t oldVal = static_cast<uint8_t>(dw[windowStart]);
            if (--freq[oldVal] == 0)
                uniqueCount--;
            windowStart++;
            windowLen--;
        }

        if (windowLen == kSboxDwords && uniqueCount == 256) {
            // Verify permutation (belt-and-suspenders with the frequency check)
            if (!IsPermutation256_32(dw + windowStart)) {
                // Shouldn't happen with uniqueCount==256, but be safe
                continue;
            }

            uintptr_t sboxAddr = base + windowStart * 4;

            RC4Candidate cand;
            cand.addr = sboxAddr;
            cand.elemSize = 4;
            cand.isEncryptState = false;
            cand.cloneValid = false;

            // Layout 0: [i32][j32][S32] — 8 bytes (2 DWORDs) before S
            if (windowStart >= 2) {
                cand.layout = 0;
                found.push_back(cand);
                LOG(INFO) << "[RC4_SCAN] Found uint32_t S-box #" << found.size()
                          << " at 0x" << std::hex << std::setfill('0')
                          << std::setw(8) << sboxAddr
                          << " (layout=[i32][j32][S32])";
            }

            // Layout 1: [S32][i32][j32] — 8 bytes after S
            if ((windowStart + kSboxDwords + 2) * 4 <= regionSize) {
                cand.layout = 1;
                found.push_back(cand);
                LOG(INFO) << "[RC4_SCAN] Found uint32_t S-box #" << found.size()
                          << " at 0x" << std::hex << std::setfill('0')
                          << std::setw(8) << sboxAddr
                          << " (layout=[S32][i32][j32])";
            }

            // Skip past this S-box
            memset(freq, 0, sizeof(freq));
            uniqueCount = 0;
            windowStart = windowStart + kSboxDwords;
            i = windowStart - 1; // loop increments to windowStart
        }
    }
}

bool ScanForRC4States()
{
    EnsureLockInitialized();
    EnterCriticalSection(&g_lock);

    g_candidates.clear();
    g_encryptIdx = -1;
    g_scanDone   = true;

    LeaveCriticalSection(&g_lock);

    LOG(INFO) << "[RC4_SCAN] Scanning readable memory for RC4 S-box permutations...";

    std::vector<RC4Candidate> found;
    size_t regionsScanned = 0;
    size_t bytesScanned   = 0;

    // Pass 1: try uint8_t S[256] (256 bytes, fast)
    EnumReadableRegions(
        [&found](uintptr_t base, const uint8_t* buf, size_t sz) {
            ScanRegion8(base, buf, sz, found);
        },
        258, // min region size for uint8_t S + i/j
        regionsScanned, bytesScanned);

    if (!found.empty()) {
        LOG(INFO) << "[RC4_SCAN] uint8_t scan: " << found.size()
                  << " candidates (" << regionsScanned << " regions, "
                  << (bytesScanned / 1024) << " KB)";
    } else {
        LOG(INFO) << "[RC4_SCAN] uint8_t scan: 0 candidates (" << regionsScanned
                  << " regions, " << (bytesScanned / 1024)
                  << " KB) — trying uint32_t S[256]...";

        // Pass 2: try uint32_t S[256] (1024 bytes)
        size_t regs2 = 0, bytes2 = 0;
        EnumReadableRegions(
            [&found](uintptr_t base, const uint8_t* buf, size_t sz) {
                ScanRegion32(base, buf, sz, found);
            },
            1024 + 8, // min region size for uint32_t S + i/j
            regs2, bytes2);

        regionsScanned += regs2;
        bytesScanned   += bytes2;

        if (!found.empty()) {
            LOG(INFO) << "[RC4_SCAN] uint32_t scan: " << found.size()
                      << " candidates (" << regs2 << " regions, "
                      << (bytes2 / 1024) << " KB)";
        } else {
            LOG(WARNING) << "[RC4_SCAN] No S-box candidates found in either format ("
                         << regionsScanned << " regions, "
                         << (bytesScanned / 1024) << " KB total)";
        }
    }

    // Commit results under lock
    EnterCriticalSection(&g_lock);
    g_candidates = std::move(found);
    LeaveCriticalSection(&g_lock);

    LOG(INFO) << "[RC4_SCAN] Scan complete: " << std::dec << g_candidates.size()
              << " candidates found (" << regionsScanned << " regions, "
              << (bytesScanned / 1024) << " KB scanned)";

    return !g_candidates.empty();
}

bool HasCandidates()
{
    return !g_candidates.empty();
}

void CloneAllStates()
{
    EnsureLockInitialized();
    EnterCriticalSection(&g_lock);

    if (g_candidates.empty()) {
        LeaveCriticalSection(&g_lock);
        return;
    }

    // If encrypt state already identified, fast path uses running state — no clone needed
    if (g_encryptIdx >= 0) {
        LeaveCriticalSection(&g_lock);
        return;
    }

    // Don't overwrite valid clones pending for identification.
    // CMSGs are queued and arrive at SendPacket AFTER subsequent PreHandlers run,
    // so we must keep the first clone set until DecryptCmsg consumes it.
    if (g_clonesReady) {
        LeaveCriticalSection(&g_lock);
        return;
    }

    int cloned = 0;

    for (auto& cand : g_candidates) {
        cand.cloneValid = false;

        if (cand.elemSize == 4) {
            // uint32_t S[256]: read 1024 bytes
            uint32_t sbox32[256];
            if (!SafeReadBytes(reinterpret_cast<const void*>(cand.addr), sbox32, sizeof(sbox32)))
                continue;

            // Extract low byte of each DWORD into snapshot.S
            bool valid = true;
            for (int i = 0; i < 256; ++i) {
                if (sbox32[i] > 255) { valid = false; break; }
                cand.snapshot.S[i] = static_cast<uint8_t>(sbox32[i]);
            }
            if (!valid || !IsPermutation256(cand.snapshot.S))
                continue;

            // Read i/j as uint32_t (take low byte)
            uint32_t ij32[2];
            if (cand.layout == 0) {
                // [i32][j32][S32] — 8 bytes before S
                if (!SafeReadBytes(reinterpret_cast<const void*>(cand.addr - 8), ij32, 8))
                    continue;
            } else {
                // [S32][i32][j32] — 8 bytes after S
                if (!SafeReadBytes(reinterpret_cast<const void*>(cand.addr + 1024), ij32, 8))
                    continue;
            }
            cand.snapshot.i = static_cast<uint8_t>(ij32[0] & 0xFF);
            cand.snapshot.j = static_cast<uint8_t>(ij32[1] & 0xFF);

        } else {
            // uint8_t S[256]: read 258 bytes (i/j + S or S + i/j)
            uint8_t raw[258];

            if (cand.layout == 0) {
                // [i][j][S[256]] — read from addr-2
                if (!SafeReadBytes(reinterpret_cast<const void*>(cand.addr - 2), raw, 258))
                    continue;
                cand.snapshot.i = raw[0];
                cand.snapshot.j = raw[1];
                memcpy(cand.snapshot.S, raw + 2, 256);
            } else {
                // [S[256]][i][j] — read from addr
                if (!SafeReadBytes(reinterpret_cast<const void*>(cand.addr), raw, 258))
                    continue;
                memcpy(cand.snapshot.S, raw, 256);
                cand.snapshot.i = raw[256];
                cand.snapshot.j = raw[257];
            }

            if (!IsPermutation256(cand.snapshot.S))
                continue;
        }

        cand.cloneValid = true;
        cloned++;
    }

    g_clonesReady = true;

    LeaveCriticalSection(&g_lock);

    LOG(INFO) << "[RC4] Cloned " << std::dec << cloned << " of "
              << g_candidates.size() << " RC4 states in WardenPreHandler";
}

bool DecryptCmsg(const uint8_t* encrypted, size_t len,
                 uint8_t* outBuf, size_t outBufSize)
{
    if (len == 0 || len > outBufSize)
        return false;

    EnsureLockInitialized();
    EnterCriticalSection(&g_lock);

    if (g_candidates.empty()) {
        LeaveCriticalSection(&g_lock);
        return false;
    }

    // Fast path: encrypt state already identified — use running state
    if (g_encryptIdx >= 0) {
        auto& cand = g_candidates[g_encryptIdx];
        if (!cand.cloneValid) {
            LeaveCriticalSection(&g_lock);
            return false;
        }

        RC4State st = cand.running;
        for (size_t i = 0; i < len; ++i)
            outBuf[i] = encrypted[i] ^ RC4NextByte(st);

        if (ValidateDecryptedCmsg(outBuf, len)) {
            cand.running = st;  // Advance running state past this CMSG
            LeaveCriticalSection(&g_lock);
            return true;
        }

        // Desync detected — force re-clone and fall through to slow path
        LOG(WARNING) << "[RC4] Encrypt state desync (byte0=0x"
                     << std::hex << std::setfill('0') << std::setw(2)
                     << (int)outBuf[0] << "), will re-clone on next handler";
        cand.running = {};  // Invalidate stale running state
        g_encryptIdx = -1;
        g_clonesReady = false;
        // NOTE: Lock NOT released — fall through to slow path for immediate retry
    }

    // Slow path: try all candidates with structural validation
    int matchCount = 0;
    int lastMatchIdx = -1;

    for (size_t idx = 0; idx < g_candidates.size(); ++idx) {
        auto& cand = g_candidates[idx];
        if (!cand.cloneValid)
            continue;

        // Quick rejection: decrypt just byte 0
        RC4State st = cand.snapshot;
        uint8_t keyByte = RC4NextByte(st);
        uint8_t firstPlain = encrypted[0] ^ keyByte;

        if (!IsValidClientOpcode(firstPlain))
            continue;

        // Full decrypt + structural validation
        st = cand.snapshot;
        for (size_t i = 0; i < len; ++i)
            outBuf[i] = encrypted[i] ^ RC4NextByte(st);

        if (!ValidateDecryptedCmsg(outBuf, len))
            continue;

        matchCount++;
        lastMatchIdx = static_cast<int>(idx);
    }

    if (matchCount == 1) {
        auto& cand = g_candidates[lastMatchIdx];
        g_encryptIdx = lastMatchIdx;
        cand.isEncryptState = true;
        cand.running = cand.snapshot;

        // Re-decrypt into outBuf (may have been overwritten by later candidates)
        // and advance running state past this CMSG
        RC4State st = cand.snapshot;
        for (size_t i = 0; i < len; ++i)
            outBuf[i] = encrypted[i] ^ RC4NextByte(st);
        cand.running = st;

        LeaveCriticalSection(&g_lock);

        size_t dumpLen = (len < 64) ? len : 64;
        LOG(INFO) << "[RC4] Identified encrypt state: candidate #"
                  << (lastMatchIdx + 1)
                  << " at 0x" << std::hex << std::setfill('0') << std::setw(8)
                  << cand.addr << " (layout=" << std::dec << cand.layout
                  << ", elem=" << cand.elemSize << ")"
                  << " decrypted=[" << BytesToHex(outBuf, dumpLen)
                  << (len > dumpLen ? " ..." : "") << "]"
                  << " warden_op=0x" << std::hex << std::setfill('0')
                  << std::setw(2) << (int)outBuf[0]
                  << " (" << WardenClientOpcodeToString(outBuf[0]) << ")";
        return true;
    }

    if (matchCount > 1) {
        LOG(WARNING) << "[RC4] Ambiguous: " << matchCount
                     << " candidates passed structural validation for len="
                     << len << " — cannot identify encrypt state";
    } else {
        // matchCount == 0: no candidate matched — clones are stale, force re-clone
        g_clonesReady = false;
        LOG(WARNING) << "[RC4] No candidate passed structural validation (len="
                     << std::dec << len << ") — will re-clone on next handler";
    }

    LeaveCriticalSection(&g_lock);
    return false;
}

bool HasEncryptState()
{
    return g_encryptIdx >= 0;
}

} // namespace warden_rc4
