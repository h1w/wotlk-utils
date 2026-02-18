#include "warden_rc4_hook.h"
#include "warden_types.h"
#include "warden_spoof.h"

#define NOMINMAX
#include <Windows.h>
#include <MinHook.h>
#include <glog/logging.h>

#include <cstring>
#include <vector>
#include <algorithm>
#include <sstream>
#include <iomanip>

// ===========================================================================
// Internal RC4 hook: hooks RC4 PRGA functions INSIDE the Warden module blob
// to capture CMSG plaintext directly before encryption.
//
// The Warden module's RC4 context uses [S[256]][i][j] layout, so i/j fields
// are at offsets +0x100 and +0x101 from the context base pointer.
// The scanner finds RC4 functions by looking for MOVZX/MOV instructions
// with these distinctive displacements.
//
// Modules may contain multiple RC4 implementations (main thread vs module
// thread), so we hook ALL viable clusters, not just the largest one.
// ===========================================================================

namespace {

static constexpr size_t kMaxCaptureSize = 4096;
static constexpr int    kMaxRC4Hooks    = 4;

// ---------------------------------------------------------------------------
// Multi-hook state: up to kMaxRC4Hooks simultaneous hooks
// ---------------------------------------------------------------------------
static void*     g_trampolines[kMaxRC4Hooks] = {};
static uintptr_t g_hookedAddrs[kMaxRC4Hooks] = {};
static bool      g_hooksActive[kMaxRC4Hooks] = {};
static int       g_numHooks = 0;

// Module memory range (for diagnostics/validation)
static uintptr_t g_moduleBase = 0;
static size_t    g_moduleSize = 0;

// ---------------------------------------------------------------------------
// Captured plaintext buffer
// ---------------------------------------------------------------------------
static uint8_t g_capturedPlaintext[kMaxCaptureSize];
static size_t  g_capturedLen   = 0;
static bool    g_capturedValid = false;

// ---------------------------------------------------------------------------
// Thread safety: RC4 called from Warden module thread, consumed from
// SendPacket on possibly same or different thread.
// ---------------------------------------------------------------------------
static CRITICAL_SECTION g_lock;
static bool g_lockInit = false;

// ---------------------------------------------------------------------------
// Calling convention detection (shared across all hook slots)
// ---------------------------------------------------------------------------
static int  g_callCount       = 0;
static bool g_conventionKnown = false;
// 0 = unknown
// 1 = __thiscall: ECX=ctx, stk1=data, stk2=len
// 2 = __cdecl:    stk1=ctx, stk2=data, stk3=len
// 3 = variant:    ECX=ctx, stk1=len, stk2=data
// 4 = EDX:        EDX=ctx, stk1=data, stk2=len
// 5 = EAX:        EAX=ctx, stk1=data, stk2=len
// 6 = EAX-variant: EAX=ctx, stk1=len, stk2=data
static int  g_convention = 0;

static void EnsureLock()
{
    if (!g_lockInit) {
        InitializeCriticalSection(&g_lock);
        g_lockInit = true;
    }
}

// ---------------------------------------------------------------------------
// SEH-safe helpers (separate functions — no C++ objects with __try/__except)
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

static bool __cdecl CheckPermutation256(const uint8_t* data)
{
    __try {
        uint8_t seen[256] = {};
        for (int i = 0; i < 256; ++i) {
            if (seen[data[i]])
                return false;
            seen[data[i]] = 1;
        }
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// ---------------------------------------------------------------------------
// Pointer / value validation helpers
// ---------------------------------------------------------------------------

static bool IsValidPointer(uint32_t val)
{
    return val >= 0x10000 && val < 0x7FFFFFFF;
}

static bool IsReasonableLength(uint32_t val)
{
    return val >= 1 && val <= kMaxCaptureSize;
}

// Check if pointer likely points to an RC4 context: [S[256]][i][j]
// S must be a permutation of 0..255.
static bool LooksLikeRC4Context(uint32_t ptr)
{
    if (!IsValidPointer(ptr))
        return false;
    uint8_t buf[256];
    if (!SafeReadBytes(reinterpret_cast<const void*>(ptr), buf, 256))
        return false;
    return CheckPermutation256(buf);
}

// ---------------------------------------------------------------------------
// CMSG structural validation (same logic as warden_rc4.cpp)
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

    case WARDEN_CMSG_MEM_CHECKS_RESULT:   // 0x03
        return len >= 1;

    case WARDEN_CMSG_HASH_RESULT:         // 0x04
        return len == 21;

    case WARDEN_CMSG_MODULE_FAILED:       // 0x05
        return len == 1;

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

// ===========================================================================
// Pattern scanner: find ALL RC4 PRGA functions in runtime module memory.
//
// Looks for instructions referencing displacements 0x100 and 0x101, which
// correspond to the i/j fields in an [S[256]][i][j] RC4 context layout.
//
// Instruction patterns scanned:
//   MOVZX r32, byte ptr [reg+disp32]  — 0F B6 [ModRM:mod=10] [disp32]
//   MOV   byte ptr [reg+disp32], r8   — 88    [ModRM:mod=10] [disp32]
//   ADD   byte ptr [reg+disp32], r8   — 00    [ModRM:mod=10] [disp32]
//   INC/DEC byte ptr [reg+disp32]     — FE    [ModRM:mod=10] [disp32]
//   LEA   r32, [reg+disp32]           — 8D    [ModRM:mod=10] [disp32]
//   ADD   byte ptr [reg+disp32], imm8 — 80    [ModRM:mod=10] [disp32] imm8
// ===========================================================================

struct PatternMatch {
    size_t   offset;
    uint32_t disp;
};

// Check if a ModRM byte has mod=10 (32-bit displacement).
// hasSIB: set if rm=4 (SIB byte present).
static bool DecodeModRM_Mod10(uint8_t modrm, bool& hasSIB)
{
    uint8_t mod = (modrm >> 6) & 3;
    uint8_t rm  = modrm & 7;
    if (mod != 2) return false;
    hasSIB = (rm == 4);
    return true;
}

static void ScanForDispReferences(const uint8_t* buf, size_t size,
                                   std::vector<PatternMatch>& matches)
{
    struct OpcodeInfo {
        uint8_t firstByte;
        uint8_t secondByte;  // 0 if single-byte opcode
        int     modrmOffset; // offset of ModRM from instruction start
    };

    static constexpr OpcodeInfo kOpcodes[] = {
        { 0x0F, 0xB6, 2 },  // MOVZX r32, r/m8
        { 0x88, 0x00, 1 },  // MOV r/m8, r8
        { 0x00, 0x00, 1 },  // ADD r/m8, r8
        { 0xFE, 0x00, 1 },  // INC/DEC r/m8
        { 0x8D, 0x00, 1 },  // LEA r32, m
        { 0x80, 0x00, 1 },  // ADD/OR/etc r/m8, imm8
    };

    for (const auto& op : kOpcodes) {
        bool isTwoByte = (op.secondByte != 0);

        for (size_t i = 0; i + 7 <= size; ++i) {
            if (buf[i] != op.firstByte)
                continue;
            if (isTwoByte && (i + 1 >= size || buf[i + 1] != op.secondByte))
                continue;

            size_t modrmPos = i + op.modrmOffset;
            if (modrmPos >= size)
                continue;

            uint8_t modrm = buf[modrmPos];
            bool hasSIB = false;
            if (!DecodeModRM_Mod10(modrm, hasSIB))
                continue;

            size_t dispPos = modrmPos + 1 + (hasSIB ? 1 : 0);
            if (dispPos + 4 > size)
                continue;

            uint32_t disp;
            std::memcpy(&disp, &buf[dispPos], 4);

            if (disp == 0x100 || disp == 0x101)
                matches.push_back({ i, disp });
        }
    }
}

// Walk backward from clusterStart to find function prologue in buf.
static size_t FindFunctionPrologue(const uint8_t* buf, size_t size, size_t clusterStart)
{
    size_t funcOffset = clusterStart;
    for (size_t back = 1; back <= 256 && back <= clusterStart; ++back) {
        size_t pos = clusterStart - back;

        // push ebp; mov ebp, esp (55 8B EC)
        if (pos + 2 < size &&
            buf[pos] == 0x55 && buf[pos + 1] == 0x8B && buf[pos + 2] == 0xEC)
        {
            if (pos == 0 ||
                buf[pos - 1] == 0xCC ||   // INT3 padding
                buf[pos - 1] == 0x90 ||   // NOP padding
                buf[pos - 1] == 0xC3 ||   // RET of prev function
                buf[pos - 1] == 0xC2)     // RET imm16 (last byte)
            {
                funcOffset = pos;
                break;
            }
            funcOffset = pos; // tentative — keep scanning
        }

        // INT3 (CC) padding before function
        if (buf[pos] == 0xCC) {
            funcOffset = pos + 1;
            break;
        }

        // RET (C3) of previous function
        if (buf[pos] == 0xC3) {
            funcOffset = pos + 1;
            break;
        }

        // RET imm16 (C2 XX XX) of previous function
        if (buf[pos] == 0xC2 && pos + 2 < clusterStart) {
            funcOffset = pos + 3;
            break;
        }
    }
    return funcOffset;
}

// Scan module memory and return ALL viable RC4 function addresses.
// Groups matches into clusters (120-byte gap between adjacent matches),
// then for each cluster with both 0x100 and 0x101 references, walks back
// to find the function prologue.
static std::vector<uintptr_t> ScanRuntimeForAllRC4(uintptr_t base, size_t size)
{
    std::vector<uintptr_t> result;

    if (size < 512) {
        LOG(INFO) << "[RC4_HOOK] Module too small (" << size << " bytes)";
        return result;
    }

    // Read module memory region-by-region (the first page(s) of the allocation
    // may have PAGE_NOACCESS or be uncommitted, so a single memcpy would fail).
    std::vector<uint8_t> buf(size, 0);
    {
        uintptr_t current = base;
        uintptr_t end = base + size;
        size_t bytesRead = 0;
        MEMORY_BASIC_INFORMATION mbi;
        while (current < end &&
               VirtualQuery(reinterpret_cast<LPCVOID>(current), &mbi, sizeof(mbi)) == sizeof(mbi))
        {
            uintptr_t regionStart = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
            uintptr_t regionEnd = regionStart + mbi.RegionSize;
            if (regionEnd > end) regionEnd = end;
            if (current < regionStart) current = regionStart;
            size_t chunkSize = static_cast<size_t>(regionEnd - current);

            if ((mbi.State == MEM_COMMIT) &&
                !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)))
            {
                size_t offset = static_cast<size_t>(current - base);
                if (SafeReadBytes(reinterpret_cast<const void*>(current), buf.data() + offset, chunkSize))
                    bytesRead += chunkSize;
            }
            current = regionEnd;
        }
        if (bytesRead == 0) {
            LOG(WARNING) << "[RC4_HOOK] No readable regions in module at 0x"
                         << std::hex << base << " (size=0x" << size << ")";
            return result;
        }
        LOG(INFO) << "[RC4_HOOK] Read 0x" << std::hex << bytesRead
                  << " of 0x" << size << " bytes from module at 0x" << base;
    }

    // Scan for instructions referencing 0x100/0x101
    std::vector<PatternMatch> matches;
    ScanForDispReferences(buf.data(), size, matches);

    if (matches.empty()) {
        LOG(INFO) << "[RC4_HOOK] No instructions referencing 0x100/0x101 found in module";
        return result;
    }

    LOG(INFO) << "[RC4_HOOK] Found " << std::dec << matches.size()
              << " instructions referencing 0x100/0x101 in module memory";

    // Sort by offset
    std::sort(matches.begin(), matches.end(),
              [](const PatternMatch& a, const PatternMatch& b) {
                  return a.offset < b.offset;
              });

    // Group matches into clusters: consecutive matches within 120 bytes of
    // each other belong to the same cluster.
    struct Cluster {
        size_t startIdx;
        size_t endIdx;   // exclusive
    };
    std::vector<Cluster> clusters;
    {
        size_t i = 0;
        while (i < matches.size()) {
            size_t j = i + 1;
            while (j < matches.size() &&
                   matches[j].offset - matches[j - 1].offset <= 120)
                ++j;
            clusters.push_back({ i, j });
            i = j;
        }
    }

    LOG(INFO) << "[RC4_HOOK] " << std::dec << clusters.size()
              << " cluster(s) found in module";

    // For each cluster: check quality and find function prologue
    for (size_t ci = 0; ci < clusters.size(); ++ci) {
        size_t count = clusters[ci].endIdx - clusters[ci].startIdx;
        size_t startIdx = clusters[ci].startIdx;
        size_t endIdx   = clusters[ci].endIdx;

        bool has100 = false, has101 = false;
        for (size_t k = startIdx; k < endIdx; ++k) {
            if (matches[k].disp == 0x100) has100 = true;
            if (matches[k].disp == 0x101) has101 = true;
        }

        size_t clusterStart = matches[startIdx].offset;
        size_t clusterEnd   = matches[endIdx - 1].offset;

        LOG(INFO) << "[RC4_HOOK] Cluster #" << std::dec << (ci + 1)
                  << ": " << count << " matches at module+0x"
                  << std::hex << clusterStart << "-0x" << clusterEnd
                  << (has100 ? " [has i]" : " [NO i]")
                  << (has101 ? " [has j]" : " [NO j]");

        // Skip clusters without both i and j references
        if (!has100 || !has101) {
            LOG(INFO) << "[RC4_HOOK]   -> skipped (missing i or j reference)";
            continue;
        }

        // Skip tiny clusters (likely false positives)
        if (count < 2) {
            LOG(INFO) << "[RC4_HOOK]   -> skipped (only " << std::dec << count << " match)";
            continue;
        }

        // Find function prologue
        size_t funcOffset = FindFunctionPrologue(buf.data(), size, clusterStart);
        uintptr_t absoluteAddr = base + funcOffset;

        // Validate prologue: must start with push ebp; mov ebp, esp (55 8B EC).
        // Without this check, FindFunctionPrologue may resolve to mid-function
        // code (e.g., SBB EAX, imm32) causing MH_ERROR_UNSUPPORTED_FUNCTION.
        if (funcOffset + 2 < size &&
            !(buf[funcOffset] == 0x55 && buf[funcOffset + 1] == 0x8B && buf[funcOffset + 2] == 0xEC))
        {
            LOG(INFO) << "[RC4_HOOK]   -> skipped (prologue at module+0x"
                      << std::hex << funcOffset << " is not 'push ebp; mov ebp, esp': ["
                      << BytesToHex(buf.data() + funcOffset,
                                    (size - funcOffset < 8) ? (size - funcOffset) : 8)
                      << "])";
            continue;
        }

        // Check for duplicates (different clusters might resolve to same function)
        bool duplicate = false;
        for (const auto& addr : result) {
            if (addr == absoluteAddr) {
                duplicate = true;
                break;
            }
        }
        if (duplicate) {
            LOG(INFO) << "[RC4_HOOK]   -> skipped (duplicate of already-found function at 0x"
                      << std::hex << absoluteAddr << ")";
            continue;
        }

        // Log prologue bytes for diagnostics
        size_t dumpLen = (size - funcOffset < 32) ? (size - funcOffset) : 32;
        LOG(INFO) << "[RC4_HOOK]   -> function at 0x"
                  << std::hex << std::setfill('0') << std::setw(8) << absoluteAddr
                  << " (module+0x" << funcOffset << ")"
                  << " prologue=[" << BytesToHex(buf.data() + funcOffset, dumpLen) << "]";

        result.push_back(absoluteAddr);

        if (result.size() >= static_cast<size_t>(kMaxRC4Hooks))
            break;
    }

    return result;
}

// ===========================================================================
// Speculative CMSG scan: when calling convention is unknown, probe all
// register and stack values to find a valid CMSG buffer.
//
// For each candidate pointer, reads 1 byte to check the CMSG opcode.
// For HASH_RESULT (0x04) expects len=21; for CHEAT_CHECKS_RESULT (0x02)
// reads the resultLen header and expects len=7+resultLen.
// Then checks if any OTHER candidate value equals the expected length.
//
// Very cheap (9 single-byte reads + integer comparisons) with strong
// validation — false positives require both a pointer to memory starting
// with 0x04/0x02 AND a matching length in another register/stack slot.
// ===========================================================================

static bool TrySpeculativeCmsgScan(
    uint32_t eax, uint32_t ecx, uint32_t edx, uint32_t ebx,
    uint32_t esi, uint32_t edi, uint32_t stk1, uint32_t stk2, uint32_t stk3,
    uint32_t& outPtr, uint32_t& outLen)
{
    uint32_t vals[] = { eax, ecx, edx, ebx, esi, edi, stk1, stk2, stk3 };
    static const char* names[] = {
        "EAX","ECX","EDX","EBX","ESI","EDI","stk1","stk2","stk3"
    };

    for (int pi = 0; pi < 9; ++pi) {
        if (!IsValidPointer(vals[pi]))
            continue;

        // Probe first byte: is it a CMSG opcode we care about?
        uint8_t opcode;
        if (!SafeReadBytes(reinterpret_cast<const void*>(vals[pi]), &opcode, 1))
            continue;

        uint32_t expectedLen = 0;
        if (opcode == WARDEN_CMSG_HASH_RESULT) {
            expectedLen = 21;
        } else if (opcode == WARDEN_CMSG_CHEAT_CHECKS_RESULT) {
            // Read 3-byte header: [0x02][resultLen:2 LE]
            uint8_t header[3];
            if (!SafeReadBytes(reinterpret_cast<const void*>(vals[pi]), header, 3))
                continue;
            uint16_t rLen = static_cast<uint16_t>(header[1]) |
                            (static_cast<uint16_t>(header[2]) << 8);
            if (rLen > kMaxCaptureSize - 7)
                continue;
            expectedLen = 7u + rLen;
        } else {
            continue;
        }

        // Check if any OTHER candidate holds the expected length
        for (int li = 0; li < 9; ++li) {
            if (li == pi) continue;
            if (vals[li] == expectedLen) {
                outPtr = vals[pi];
                outLen = expectedLen;
                LOG(INFO) << "[RC4_HOOK] Speculative scan: found "
                          << WardenClientOpcodeToString(opcode)
                          << " at " << names[pi] << "=0x" << std::hex
                          << std::setfill('0') << std::setw(8) << vals[pi]
                          << " len=" << names[li] << "=" << std::dec << expectedLen;
                return true;
            }
        }
    }
    return false;
}

// ===========================================================================
// Naked hook stubs and detour handler
// ===========================================================================

// Stack layout after pushad+pushfd (36 bytes):
//   s[0]=EFLAGS s[1]=EDI s[2]=ESI s[3]=EBP s[4]=ESP_orig
//   s[5]=EBX s[6]=EDX s[7]=ECX s[8]=EAX
//   s[9]=retaddr s[10]=stk1 s[11]=stk2 s[12]=stk3

static void __cdecl RC4DetourHandler(uintptr_t savedEsp)
{
    uint32_t* s = reinterpret_cast<uint32_t*>(savedEsp);

    uint32_t eax  = s[8];
    uint32_t ecx  = s[7];
    uint32_t edx  = s[6];
    uint32_t ebx  = s[5];
    uint32_t esi  = s[2];
    uint32_t edi  = s[1];
    uint32_t stk1 = s[10];
    uint32_t stk2 = s[11];
    uint32_t stk3 = s[12];

    int callNum = InterlockedIncrement(reinterpret_cast<volatile LONG*>(&g_callCount));

    // Diagnostic logging for first few calls — dump ALL registers + stack args
    if (callNum <= 8) {
        LOG(INFO) << "[RC4_HOOK] call#" << std::dec << callNum
                  << " EAX=0x" << std::hex << std::setfill('0') << std::setw(8) << eax
                  << " ECX=0x" << std::setw(8) << ecx
                  << " EDX=0x" << std::setw(8) << edx
                  << " EBX=0x" << std::setw(8) << ebx
                  << " ESI=0x" << std::setw(8) << esi
                  << " EDI=0x" << std::setw(8) << edi
                  << " stk1=0x" << std::setw(8) << stk1
                  << " stk2=0x" << std::setw(8) << stk2
                  << " stk3=0x" << std::setw(8) << stk3;
    }

    // --- Calling convention auto-detection ---
    // Order: register-based (most specific) before stack-based (least specific)
    // to avoid false positives where a stack arg coincidentally looks like an S-box.
    if (!g_conventionKnown) {
        // Try __thiscall: ECX=ctx, stk1=data, stk2=len
        if (LooksLikeRC4Context(ecx) &&
            IsValidPointer(stk1) && IsReasonableLength(stk2))
        {
            g_convention = 1;
            g_conventionKnown = true;
            LOG(INFO) << "[RC4_HOOK] Convention detected: __thiscall"
                      << " (ECX=ctx, stk1=data, stk2=len)";
        }
        // Try variant: ECX=ctx, stk1=len, stk2=data
        else if (LooksLikeRC4Context(ecx) &&
                 IsReasonableLength(stk1) && IsValidPointer(stk2))
        {
            g_convention = 3;
            g_conventionKnown = true;
            LOG(INFO) << "[RC4_HOOK] Convention detected: variant"
                      << " (ECX=ctx, stk1=len, stk2=data)";
        }
        // Try: EDX=ctx, stk1=data, stk2=len
        else if (LooksLikeRC4Context(edx) &&
                 IsValidPointer(stk1) && IsReasonableLength(stk2))
        {
            g_convention = 4;
            g_conventionKnown = true;
            LOG(INFO) << "[RC4_HOOK] Convention detected: EDX-variant"
                      << " (EDX=ctx, stk1=data, stk2=len)";
        }
        // Try: EAX=ctx, stk1=data, stk2=len
        else if (LooksLikeRC4Context(eax) &&
                 IsValidPointer(stk1) && IsReasonableLength(stk2))
        {
            g_convention = 5;
            g_conventionKnown = true;
            LOG(INFO) << "[RC4_HOOK] Convention detected: EAX-variant"
                      << " (EAX=ctx, stk1=data, stk2=len)";
        }
        // Try: EAX=ctx, stk1=len, stk2=data
        else if (LooksLikeRC4Context(eax) &&
                 IsReasonableLength(stk1) && IsValidPointer(stk2))
        {
            g_convention = 6;
            g_conventionKnown = true;
            LOG(INFO) << "[RC4_HOOK] Convention detected: EAX-variant2"
                      << " (EAX=ctx, stk1=len, stk2=data)";
        }
        // Try __cdecl: stk1=ctx, stk2=data, stk3=len (least specific — last)
        else if (LooksLikeRC4Context(stk1) &&
                 IsValidPointer(stk2) && IsReasonableLength(stk3))
        {
            g_convention = 2;
            g_conventionKnown = true;
            LOG(INFO) << "[RC4_HOOK] Convention detected: __cdecl"
                      << " (stk1=ctx, stk2=data, stk3=len)";
        }
    }

    // --- Extract parameters based on known convention ---
    uint32_t dataPtr = 0, dataLen = 0;

    switch (g_convention) {
    case 1: dataPtr = stk1; dataLen = stk2; break;  // ECX=ctx
    case 2: dataPtr = stk2; dataLen = stk3; break;  // stk1=ctx
    case 3: dataPtr = stk2; dataLen = stk1; break;  // ECX=ctx, swapped
    case 4: dataPtr = stk1; dataLen = stk2; break;  // EDX=ctx
    case 5: dataPtr = stk1; dataLen = stk2; break;  // EAX=ctx
    case 6: dataPtr = stk2; dataLen = stk1; break;  // EAX=ctx, swapped
    default:
        // Convention unknown — speculative CMSG scan first (validated),
        // then heuristic fallback for other CMSG types.
        TrySpeculativeCmsgScan(eax, ecx, edx, ebx, esi, edi,
                               stk1, stk2, stk3, dataPtr, dataLen);
        // Heuristic fallback for 1-byte CMSGs (MODULE_MISSING/OK/FAILED)
        if (!IsValidPointer(dataPtr) || !IsReasonableLength(dataLen)) {
            if (IsValidPointer(stk1) && IsReasonableLength(stk2)) {
                dataPtr = stk1; dataLen = stk2;
            } else if (IsValidPointer(stk2) && IsReasonableLength(stk3)) {
                dataPtr = stk2; dataLen = stk3;
            } else if (IsReasonableLength(stk1) && IsValidPointer(stk2)) {
                dataPtr = stk2; dataLen = stk1;
            }
        }
        break;
    }

    // Fallback: if convention produced invalid values (e.g., different RC4
    // functions within the same module use different calling conventions),
    // try speculative CMSG scan across all registers/stack values.
    if (!IsValidPointer(dataPtr) || !IsReasonableLength(dataLen)) {
        if (!TrySpeculativeCmsgScan(eax, ecx, edx, ebx, esi, edi,
                                     stk1, stk2, stk3, dataPtr, dataLen))
            return;
    }

    // Read data buffer BEFORE the original RC4 function modifies it.
    // For encrypt calls: this is the plaintext CMSG.
    // For decrypt calls: this is the encrypted SMSG (will fail validation).
    uint8_t localBuf[kMaxCaptureSize];
    if (!SafeReadBytes(reinterpret_cast<const void*>(dataPtr), localBuf, dataLen))
        return;

    // Validate as CMSG — only encrypt calls produce valid CMSG structure
    if (!ValidateDecryptedCmsg(localBuf, dataLen))
        return;

    // Spoof HASH_RESULT if needed (before encryption)
    if (localBuf[0] == WARDEN_CMSG_HASH_RESULT && dataLen == 21) {
        if (warden_spoof::SpoofHashResultIfNeeded(localBuf, dataLen))
            SafeReadBytes(localBuf, reinterpret_cast<void*>(dataPtr), dataLen);
    }

    // Spoof MEM_CHECK/PAGE_CHECK results BEFORE encryption.
    // Modify localBuf in-place, then write back to original buffer.
    if (warden_spoof::SpoofCmsgIfNeeded(localBuf, dataLen)) {
        // Write spoofed plaintext back to the original buffer so RC4
        // encrypts the modified data (module's buffer is writable).
        SafeReadBytes(localBuf, reinterpret_cast<void*>(dataPtr), dataLen);
    }

    // Capture (possibly spoofed) plaintext
    EnterCriticalSection(&g_lock);
    std::memcpy(g_capturedPlaintext, localBuf, dataLen);
    g_capturedLen   = dataLen;
    g_capturedValid = true;
    LeaveCriticalSection(&g_lock);

    if (callNum <= 10 || (callNum % 100 == 0)) {
        size_t dumpLen = (dataLen < 64) ? dataLen : 64;
        LOG(INFO) << "[RC4_HOOK] Captured CMSG plaintext: len=" << std::dec << dataLen
                  << " warden_op=0x" << std::hex << std::setfill('0') << std::setw(2)
                  << static_cast<int>(localBuf[0])
                  << " (" << WardenClientOpcodeToString(localBuf[0]) << ")"
                  << " data=[" << BytesToHex(localBuf, dumpLen)
                  << (dataLen > dumpLen ? " ..." : "") << "]";
    }
}

// Each hook slot needs its own naked stub that jumps to its own trampoline.
// MSVC inline asm allows static array indexing with compile-time offsets.

__declspec(naked) static void HookedRC4Naked_0()
{
    __asm {
        pushad
        pushfd
        mov eax, esp
        push eax
        call RC4DetourHandler
        add esp, 4
        popfd
        popad
        jmp dword ptr [g_trampolines + 0]
    }
}

__declspec(naked) static void HookedRC4Naked_1()
{
    __asm {
        pushad
        pushfd
        mov eax, esp
        push eax
        call RC4DetourHandler
        add esp, 4
        popfd
        popad
        jmp dword ptr [g_trampolines + 4]
    }
}

__declspec(naked) static void HookedRC4Naked_2()
{
    __asm {
        pushad
        pushfd
        mov eax, esp
        push eax
        call RC4DetourHandler
        add esp, 4
        popfd
        popad
        jmp dword ptr [g_trampolines + 8]
    }
}

__declspec(naked) static void HookedRC4Naked_3()
{
    __asm {
        pushad
        pushfd
        mov eax, esp
        push eax
        call RC4DetourHandler
        add esp, 4
        popfd
        popad
        jmp dword ptr [g_trampolines + 12]
    }
}

typedef void (*NakedHookFn)();
static NakedHookFn g_nakedStubs[kMaxRC4Hooks] = {
    HookedRC4Naked_0,
    HookedRC4Naked_1,
    HookedRC4Naked_2,
    HookedRC4Naked_3,
};

} // anonymous namespace

// ===========================================================================
// Public API
// ===========================================================================

namespace warden_rc4_hook {

void Reset()
{
    EnsureLock();
    EnterCriticalSection(&g_lock);

    g_capturedValid    = false;
    g_capturedLen      = 0;
    g_callCount        = 0;
    g_conventionKnown  = false;
    g_convention       = 0;

    LeaveCriticalSection(&g_lock);
    LOG(INFO) << "[RC4_HOOK] State reset";
}

bool Install(uintptr_t moduleBase, size_t moduleSize)
{
    EnsureLock();

    // Remove any existing hooks first
    if (g_numHooks > 0) {
        LOG(INFO) << "[RC4_HOOK] " << g_numHooks
                  << " hook(s) already active, removing before reinstall";
        Remove();
    }

    g_moduleBase = moduleBase;
    g_moduleSize = moduleSize;

    // Reset capture state for new module
    EnterCriticalSection(&g_lock);
    g_capturedValid   = false;
    g_capturedLen     = 0;
    g_callCount       = 0;
    g_conventionKnown = false;
    g_convention      = 0;
    LeaveCriticalSection(&g_lock);

    std::vector<uintptr_t> funcAddrs = ScanRuntimeForAllRC4(moduleBase, moduleSize);
    if (funcAddrs.empty()) {
        LOG(WARNING) << "[RC4_HOOK] No RC4 functions found in module at 0x"
                     << std::hex << moduleBase << " (size=0x" << moduleSize << ")";
        return false;
    }

    int installed = 0;
    for (size_t i = 0; i < funcAddrs.size() && installed < kMaxRC4Hooks; ++i) {
        uintptr_t funcAddr = funcAddrs[i];

        MH_STATUS status = MH_CreateHook(
            reinterpret_cast<LPVOID>(funcAddr),
            reinterpret_cast<LPVOID>(g_nakedStubs[installed]),
            &g_trampolines[installed]);

        if (status != MH_OK) {
            LOG(ERROR) << "[RC4_HOOK] MH_CreateHook(0x" << std::hex << funcAddr
                       << ") failed: " << MH_StatusToString(status);
            continue;
        }

        status = MH_EnableHook(reinterpret_cast<LPVOID>(funcAddr));
        if (status != MH_OK) {
            LOG(ERROR) << "[RC4_HOOK] MH_EnableHook(0x" << std::hex << funcAddr
                       << ") failed: " << MH_StatusToString(status);
            MH_RemoveHook(reinterpret_cast<LPVOID>(funcAddr));
            g_trampolines[installed] = nullptr;
            continue;
        }

        g_hookedAddrs[installed] = funcAddr;
        g_hooksActive[installed] = true;
        ++installed;

        LOG(INFO) << "[RC4_HOOK] Hook #" << installed << " installed at 0x"
                  << std::hex << std::setfill('0') << std::setw(8) << funcAddr
                  << " (module base=0x" << std::setw(8) << moduleBase
                  << " size=0x" << moduleSize << ")";
    }

    g_numHooks = installed;

    if (installed == 0) {
        LOG(WARNING) << "[RC4_HOOK] Failed to install any hooks";
        return false;
    }

    LOG(INFO) << "[RC4_HOOK] " << installed << " hook(s) installed successfully"
              << " (out of " << funcAddrs.size() << " candidate(s))";
    return true;
}

void Remove()
{
    for (int i = 0; i < kMaxRC4Hooks; ++i) {
        if (!g_hooksActive[i])
            continue;

        MH_STATUS status = MH_DisableHook(reinterpret_cast<LPVOID>(g_hookedAddrs[i]));
        if (status != MH_OK) {
            LOG(WARNING) << "[RC4_HOOK] MH_DisableHook(0x" << std::hex << g_hookedAddrs[i]
                         << ") failed: " << MH_StatusToString(status);
        }

        status = MH_RemoveHook(reinterpret_cast<LPVOID>(g_hookedAddrs[i]));
        if (status != MH_OK) {
            LOG(WARNING) << "[RC4_HOOK] MH_RemoveHook(0x" << std::hex << g_hookedAddrs[i]
                         << ") failed: " << MH_StatusToString(status);
        }

        g_hooksActive[i]  = false;
        g_hookedAddrs[i]  = 0;
        g_trampolines[i]  = nullptr;
    }

    LOG(INFO) << "[RC4_HOOK] All hooks removed (total calls: " << std::dec << g_callCount << ")";
    g_numHooks = 0;
}

bool IsActive()
{
    return g_numHooks > 0;
}

bool ConsumePlaintext(uint8_t* out, size_t outSize, size_t* outLen)
{
    EnsureLock();
    EnterCriticalSection(&g_lock);

    if (!g_capturedValid || g_capturedLen == 0 || g_capturedLen > outSize) {
        LeaveCriticalSection(&g_lock);
        return false;
    }

    std::memcpy(out, g_capturedPlaintext, g_capturedLen);
    *outLen = g_capturedLen;

    g_capturedValid = false;
    g_capturedLen   = 0;

    LeaveCriticalSection(&g_lock);
    return true;
}

void Cleanup()
{
    if (g_lockInit) {
        DeleteCriticalSection(&g_lock);
        g_lockInit = false;
    }
}

} // namespace warden_rc4_hook
