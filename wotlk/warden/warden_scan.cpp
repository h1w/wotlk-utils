#include "warden_scan.h"

#define NOMINMAX
#include <Windows.h>
#include <glog/logging.h>

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <algorithm>
#include <sstream>
#include <iomanip>

namespace warden_scan {

namespace {

// Window size to scan for cmp al, imm8 cluster (memory scan)
constexpr size_t kDispatcherWindow = 256;
// Wider window for binary scan (decompressed module, no noise)
constexpr size_t kBinaryDispatcherWindow = 512;
// Minimum unique type IDs to consider it the dispatcher (memory scan)
// Must be high enough to avoid false positives from normal x86 code
constexpr size_t kMinUniqueTypes = 8;
// Threshold for binary scan — must be high enough to avoid false positives
// from normal x86 code in the RLE-packed module. Real Warden has 9 check types.
constexpr size_t kBinaryMinUniqueTypes = 7;
// Max region size to scan (64 MB — some Warden allocations are large)
constexpr size_t kMaxRegionSize = 64 * 1024 * 1024;

// Known fixed data sizes for Warden check types (from AzerothCore source, verified by RE).
// Ordered largest-first for structural validation (more constraints = fewer false matches).
constexpr int kKnownSizes[] = { 31, 29, 25, 24, 6, 1, 0 };
constexpr size_t kNumKnownSizes = sizeof(kKnownSizes) / sizeof(kKnownSizes[0]);

// State
bool g_hasTypeIDs = false;
bool g_allSizesKnown = false;
std::unordered_set<uint8_t> g_typeIDs;
std::unordered_map<uint8_t, int> g_typeSizes; // type -> data size (-1 = unknown)
size_t g_stringCount = 0; // number of strings in current packet (for index validation)

// Cached module runtime address/size (set by FindModuleInMemory)
uintptr_t g_moduleRuntimeBase = 0;
size_t    g_moduleRuntimeSize = 0;

// Base address of the currently scanned buffer (for adjusting absolute displacements
// in relocated in-memory modules). 0 = scanning packed binary (offsets are raw file offsets).
uintptr_t g_scanBaseAddr = 0;

// ---------------------------------------------------------------------------
// SEH-safe memory copy (separate function — no C++ objects allowed with SEH)
// ---------------------------------------------------------------------------
bool __cdecl SafeMemcpy(void* dst, const void* src, size_t len)
{
    __try {
        memcpy(dst, src, len);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// ---------------------------------------------------------------------------
// Shared logic: scan a byte buffer for the Warden dispatcher pattern.
// Looks for clusters of `cmp al, imm8` (0x3C XX), `cmp reg8, imm8`
// (0x80 F8-FB XX), and `cmp eax, imm32` (0x3D XX XX XX XX) instructions.
//
// windowSize = sliding window for cluster detection
// minUnique  = minimum unique imm8 values to declare a match
// outOffset  = if non-null, receives the byte offset of the first cmp in the window
//
// Returns true if a dispatcher-like cluster was found and g_typeIDs populated.
// ---------------------------------------------------------------------------
bool ScanBufferForDispatcher(const uint8_t* buf, size_t bufSize,
                             size_t windowSize, size_t minUnique,
                             size_t* outOffset)
{
    if (bufSize < windowSize)
        return false;

    // Find all cmp reg, imm instructions:
    //   0x3C XX              — cmp al, imm8   (2 bytes)
    //   0x80 [F8-FF] XX      — cmp r8, imm8   (3 bytes)
    //   0x3D XX 00 00 00     — cmp eax, imm32  (5 bytes, upper 3 bytes zero)
    //   0x83 [F8-FF] XX      — cmp r32, sign-ext imm8  (3 bytes, imm < 0x80 only)
    //   0x81 [F8-FF] XX 0000 — cmp r32, imm32  (6 bytes, upper 3 bytes zero)
    struct CmpEntry { size_t pos; uint8_t imm; };
    std::vector<CmpEntry> cmps;
    for (size_t i = 0; i + 1 < bufSize; ++i) {
        if (buf[i] == 0x3C) {
            // cmp al, imm8
            cmps.push_back({ i, buf[i + 1] });
        } else if (buf[i] == 0x3D && i + 4 < bufSize) {
            // cmp eax, imm32 — only treat as type ID if upper 3 bytes are 0
            if (buf[i + 2] == 0 && buf[i + 3] == 0 && buf[i + 4] == 0)
                cmps.push_back({ i, buf[i + 1] });
        } else if (buf[i] == 0x80 && i + 2 < bufSize &&
                   buf[i + 1] >= 0xF8 && buf[i + 1] <= 0xFF) {
            // cmp r8, imm8 (al/cl/dl/bl/ah/ch/dh/bh)
            cmps.push_back({ i, buf[i + 2] });
        } else if (buf[i] == 0x83 && i + 2 < bufSize &&
                   buf[i + 1] >= 0xF8 && buf[i + 1] <= 0xFF) {
            // cmp r32, sign-extended imm8
            // Sign extension: 0x00-0x7F stays the same, 0x80-0xFF becomes 0xFFFFFFxx
            // Only valid for check type IDs < 0x80
            uint8_t imm = buf[i + 2];
            if (imm < 0x80)
                cmps.push_back({ i, imm });
        } else if (buf[i] == 0x81 && i + 5 < bufSize &&
                   buf[i + 1] >= 0xF8 && buf[i + 1] <= 0xFF) {
            // cmp r32, imm32 — only treat as type ID if upper 3 bytes are 0
            if (buf[i + 3] == 0 && buf[i + 4] == 0 && buf[i + 5] == 0)
                cmps.push_back({ i, buf[i + 2] });
        }
    }

    if (cmps.size() < minUnique)
        return false;

    // Sliding window: find cluster of minUnique+ unique imm8 values
    uint8_t freq[256] = {};
    int uniqueCount = 0;
    size_t lo = 0;

    for (size_t hi = 0; hi < cmps.size(); ++hi) {
        if (freq[cmps[hi].imm]++ == 0)
            uniqueCount++;

        while (cmps[hi].pos - cmps[lo].pos >= windowSize) {
            if (--freq[cmps[lo].imm] == 0)
                uniqueCount--;
            lo++;
        }

        if (uniqueCount >= static_cast<int>(minUnique)) {
            // Collect all unique type IDs from the window
            g_typeIDs.clear();
            for (int v = 0; v < 256; ++v) {
                if (freq[v] > 0)
                    g_typeIDs.insert(static_cast<uint8_t>(v));
            }

            g_typeSizes.clear();
            for (uint8_t id : g_typeIDs)
                g_typeSizes[id] = -1;

            g_hasTypeIDs = true;
            g_allSizesKnown = false;

            if (outOffset)
                *outOffset = cmps[lo].pos;

            return true;
        }
    }

    return false;
}

// ---------------------------------------------------------------------------
// Detect sub/je dispatcher chains (alternative MSVC pattern).
//
// MSVC sometimes compiles a switch on sparse values as a subtraction chain:
//   sub eax, delta0; je handler0     → type0 = delta0
//   sub eax, delta1; je handler1     → type1 = delta0 + delta1
//   sub eax, delta2; je handler2     → type2 = delta0 + delta1 + delta2
//
// The actual check type IDs are reconstructed by cumulative addition.
//
// Patterns detected:
//   2C XX               — sub al, imm8   (2 bytes)
//   83 [E8-EB] XX       — sub eax/ecx/edx/ebx, imm8  (3 bytes)
//   2D XX 00 00 00      — sub eax, imm32 (5 bytes, byte-range)
// Each followed within 0-4 bytes by:
//   74 XX / 75 XX       — je/jne rel8
//   0F 84 XX XX XX XX   — je rel32
//   0F 85 XX XX XX XX   — jne rel32
// ---------------------------------------------------------------------------
bool ScanBufferForSubChain(const uint8_t* buf, size_t bufSize,
                            size_t windowSize, size_t minPairs,
                            size_t* outOffset)
{
    struct SubJePair { size_t pos; uint8_t delta; size_t instrLen; };
    std::vector<SubJePair> pairs;

    auto hasCondJump = [&](size_t afterSub) -> bool {
        // Check up to 4 bytes after sub instruction end for a conditional jump
        for (size_t j = afterSub; j < afterSub + 5 && j < bufSize; ++j) {
            if (buf[j] == 0x74 || buf[j] == 0x75) // je/jne rel8
                return true;
            if (buf[j] == 0x0F && j + 1 < bufSize &&
                (buf[j + 1] == 0x84 || buf[j + 1] == 0x85)) // je/jne rel32
                return true;
        }
        return false;
    };

    for (size_t i = 0; i + 1 < bufSize; ++i) {
        if (buf[i] == 0x2C) {
            // sub al, imm8 (2 bytes)
            if (hasCondJump(i + 2))
                pairs.push_back({ i, buf[i + 1], 2 });
        } else if (buf[i] == 0x83 && i + 2 < bufSize &&
                   buf[i + 1] >= 0xE8 && buf[i + 1] <= 0xEB) {
            // sub eax/ecx/edx/ebx, imm8 (3 bytes)
            if (hasCondJump(i + 3))
                pairs.push_back({ i, buf[i + 2], 3 });
        } else if (buf[i] == 0x2D && i + 4 < bufSize) {
            // sub eax, imm32 — only byte-range values
            if (buf[i + 2] == 0 && buf[i + 3] == 0 && buf[i + 4] == 0) {
                if (hasCondJump(i + 5))
                    pairs.push_back({ i, buf[i + 1], 5 });
            }
        }
    }

    if (pairs.size() < minPairs)
        return false;

    // Sliding window: find cluster of minPairs+ sub/je pairs
    for (size_t lo = 0; lo + minPairs <= pairs.size(); ++lo) {
        size_t hi = lo;
        while (hi < pairs.size() && pairs[hi].pos - pairs[lo].pos < windowSize)
            ++hi;

        size_t count = hi - lo;
        if (count < minPairs)
            continue;

        // Reconstruct type IDs by cumulative addition of deltas
        std::unordered_set<uint8_t> types;
        uint32_t cumulative = 0;
        for (size_t j = lo; j < hi; ++j) {
            cumulative += pairs[j].delta;
            types.insert(static_cast<uint8_t>(cumulative & 0xFF));
        }

        if (types.size() >= minPairs) {
            g_typeIDs = types;
            g_typeSizes.clear();
            for (uint8_t id : g_typeIDs)
                g_typeSizes[id] = -1;

            g_hasTypeIDs = true;
            g_allSizesKnown = false;

            if (outOffset)
                *outOffset = pairs[lo].pos;

            return true;
        }
    }

    return false;
}

// ---------------------------------------------------------------------------
// Strategy 3: XOR-anchored dispatch chain detection (primary for binary scan)
//
// Anchors on the XOR instruction that decodes the check type byte:
//   32 [ModRM: 0x40-0x7F, rm!=4] 04   =   xor r8, byte ptr [reg + 4]
//
// Then classifies what follows:
//   - Remap table (response builder): movzx byte [reg+large_disp] + jmp [reg*4+disp]
//   - Dispatch chain (request parser): sub/dec/cmp + je/jne chain -> extract types
//
// Proven by Python/capstone analysis across 10 Warden modules.
// ---------------------------------------------------------------------------

constexpr size_t kMaxMovzxDist = 18;     // max bytes between XOR and movzx r32, r8
constexpr size_t kRemapCheckDist = 60;   // max bytes to search for remap table pattern
constexpr size_t kMaxJumpDist = 6;       // max bytes between cmp/sub end and conditional jump
constexpr size_t kChainScanLen = 400;    // max bytes to scan per branch of dispatch chain
constexpr size_t kMaxBranches = 16;      // max branches to follow in dispatch tree
constexpr size_t kMinChainTypes = 3;     // minimum types to accept as valid dispatcher

constexpr size_t kNoMatch = static_cast<size_t>(-1);

enum CondJumpKind {
    CJ_EQUAL = 0,      // je  (0x74, 0F 84)
    CJ_NOT_EQUAL = 1,  // jne (0x75, 0F 85)
    CJ_GREATER = 2,    // jg  (0x7F, 0F 8F) — signed strict >
    CJ_GREATER_EQ = 3, // jge (0x7D, 0F 8D) — signed >=
    CJ_LESS = 4,       // jl  (0x7C, 0F 8C) — signed strict <
    CJ_LESS_EQ = 5,    // jle (0x7E, 0F 8E) — signed <=
    CJ_ABOVE = 6,      // ja  (0x77, 0F 87) — unsigned strict >
    CJ_ABOVE_EQ = 7,   // jae (0x73, 0F 83) — unsigned >=
    CJ_BELOW = 8,      // jb  (0x72, 0F 82) — unsigned strict <
    CJ_BELOW_EQ = 9,   // jbe (0x76, 0F 86) — unsigned <=
};

bool IsStrictGreater(CondJumpKind k) { return k == CJ_GREATER || k == CJ_ABOVE; }
bool IsGreaterOrEq(CondJumpKind k) { return k == CJ_GREATER_EQ || k == CJ_ABOVE_EQ; }
bool IsStrictLess(CondJumpKind k) { return k == CJ_LESS || k == CJ_BELOW; }
bool IsLessOrEq(CondJumpKind k) { return k == CJ_LESS_EQ || k == CJ_BELOW_EQ; }
bool IsGreaterFamily(CondJumpKind k) { return IsStrictGreater(k) || IsGreaterOrEq(k); }
bool IsLessFamily(CondJumpKind k) { return IsStrictLess(k) || IsLessOrEq(k); }

// Find first conditional jump within dist bytes of startOff
size_t FindCondJump(const uint8_t* data, size_t dataSize,
                    size_t startOff, size_t dist, CondJumpKind* outKind)
{
    size_t end = std::min(startOff + dist, dataSize);
    for (size_t i = startOff; i < end; ++i) {
        CondJumpKind k;
        bool found = true;
        switch (data[i]) {
        case 0x74: k = CJ_EQUAL; break;
        case 0x75: k = CJ_NOT_EQUAL; break;
        case 0x7F: k = CJ_GREATER; break;
        case 0x7D: k = CJ_GREATER_EQ; break;
        case 0x7C: k = CJ_LESS; break;
        case 0x7E: k = CJ_LESS_EQ; break;
        case 0x77: k = CJ_ABOVE; break;
        case 0x73: k = CJ_ABOVE_EQ; break;
        case 0x72: k = CJ_BELOW; break;
        case 0x76: k = CJ_BELOW_EQ; break;
        case 0x0F:
            if (i + 1 < end) {
                switch (data[i + 1]) {
                case 0x84: k = CJ_EQUAL; break;
                case 0x85: k = CJ_NOT_EQUAL; break;
                case 0x8F: k = CJ_GREATER; break;
                case 0x8D: k = CJ_GREATER_EQ; break;
                case 0x8C: k = CJ_LESS; break;
                case 0x8E: k = CJ_LESS_EQ; break;
                case 0x87: k = CJ_ABOVE; break;
                case 0x83: k = CJ_ABOVE_EQ; break;
                case 0x82: k = CJ_BELOW; break;
                case 0x86: k = CJ_BELOW_EQ; break;
                default: found = false; break;
                }
            } else { found = false; }
            break;
        default: found = false; break;
        }
        if (found) {
            if (outKind) *outKind = k;
            return i;
        }
    }
    return kNoMatch;
}

// Compute target file offset from a conditional jump at jmpOff
size_t ComputeJumpTarget(const uint8_t* data, size_t dataSize, size_t jmpOff)
{
    if (jmpOff >= dataSize) return kNoMatch;
    uint8_t op = data[jmpOff];

    // Short: 7x rel8
    if (op >= 0x70 && op <= 0x7F && jmpOff + 1 < dataSize) {
        int8_t disp = static_cast<int8_t>(data[jmpOff + 1]);
        intptr_t target = static_cast<intptr_t>(jmpOff) + 2 + disp;
        if (target >= 0 && static_cast<size_t>(target) < dataSize)
            return static_cast<size_t>(target);
    }
    // Near: 0F 8x rel32
    else if (op == 0x0F && jmpOff + 5 < dataSize) {
        int32_t disp;
        std::memcpy(&disp, data + jmpOff + 2, 4);
        intptr_t target = static_cast<intptr_t>(jmpOff) + 6 + disp;
        if (target >= 0 && static_cast<size_t>(target) < dataSize)
            return static_cast<size_t>(target);
    }
    return kNoMatch;
}

// Byte length of conditional jump instruction at jmpOff
size_t CondJumpLen(const uint8_t* data, size_t jmpOff)
{
    return (data[jmpOff] == 0x0F) ? 6 : 2;
}

// ---------------------------------------------------------------------------
// Minimal x86 (32-bit mode) instruction length decoder.
// Returns instruction length at data[0], or 1 as fallback for unknown opcodes.
// Handles the common instruction forms found in Warden module dispatch code.
// ---------------------------------------------------------------------------
size_t X86InsnLen(const uint8_t* data, size_t maxLen)
{
    if (maxLen == 0) return 0;
    size_t pos = 0;

    // Skip legacy prefixes
    while (pos < maxLen) {
        uint8_t b = data[pos];
        if (b == 0x66 || b == 0x67 || b == 0xF2 || b == 0xF3 || b == 0xF0 ||
            b == 0x26 || b == 0x2E || b == 0x36 || b == 0x3E || b == 0x64 || b == 0x65)
            pos++;
        else
            break;
    }
    if (pos >= maxLen) return pos ? pos : 1;

    uint8_t op = data[pos++];
    bool has_modrm = false;
    int imm_size = 0;

    switch (op) {
    // ALU r/m, r and r, r/m (ModRM, no imm)
    case 0x00: case 0x01: case 0x02: case 0x03: // add
    case 0x08: case 0x09: case 0x0A: case 0x0B: // or
    case 0x10: case 0x11: case 0x12: case 0x13: // adc
    case 0x18: case 0x19: case 0x1A: case 0x1B: // sbb
    case 0x20: case 0x21: case 0x22: case 0x23: // and
    case 0x28: case 0x29: case 0x2A: case 0x2B: // sub
    case 0x30: case 0x31: case 0x32: case 0x33: // xor
    case 0x38: case 0x39: case 0x3A: case 0x3B: // cmp
        has_modrm = true; break;
    // ALU AL, imm8
    case 0x04: case 0x0C: case 0x14: case 0x1C:
    case 0x24: case 0x2C: case 0x34: case 0x3C:
        imm_size = 1; break;
    // ALU EAX, imm32
    case 0x05: case 0x0D: case 0x15: case 0x1D:
    case 0x25: case 0x2D: case 0x35: case 0x3D:
        imm_size = 4; break;
    // inc/dec r32
    case 0x40: case 0x41: case 0x42: case 0x43:
    case 0x44: case 0x45: case 0x46: case 0x47:
    case 0x48: case 0x49: case 0x4A: case 0x4B:
    case 0x4C: case 0x4D: case 0x4E: case 0x4F:
        break;
    // push/pop r32
    case 0x50: case 0x51: case 0x52: case 0x53:
    case 0x54: case 0x55: case 0x56: case 0x57:
    case 0x58: case 0x59: case 0x5A: case 0x5B:
    case 0x5C: case 0x5D: case 0x5E: case 0x5F:
        break;
    case 0x68: imm_size = 4; break; // push imm32
    case 0x6A: imm_size = 1; break; // push imm8
    // short Jcc
    case 0x70: case 0x71: case 0x72: case 0x73:
    case 0x74: case 0x75: case 0x76: case 0x77:
    case 0x78: case 0x79: case 0x7A: case 0x7B:
    case 0x7C: case 0x7D: case 0x7E: case 0x7F:
        imm_size = 1; break;
    // Group 1: r/m, imm
    case 0x80: case 0x82: has_modrm = true; imm_size = 1; break;
    case 0x81: has_modrm = true; imm_size = 4; break;
    case 0x83: has_modrm = true; imm_size = 1; break;
    // test/xchg/mov with ModRM
    case 0x84: case 0x85: case 0x86: case 0x87:
    case 0x88: case 0x89: case 0x8A: case 0x8B:
    case 0x8C: case 0x8D: case 0x8E:
        has_modrm = true; break;
    case 0x8F: has_modrm = true; break; // pop r/m
    // nop / xchg eax, r32
    case 0x90: case 0x91: case 0x92: case 0x93:
    case 0x94: case 0x95: case 0x96: case 0x97:
        break;
    case 0x98: case 0x99: break; // cbw/cwd
    case 0x9C: case 0x9D: break; // pushfd/popfd
    case 0x9E: case 0x9F: break; // sahf/lahf
    case 0xA0: case 0xA2: imm_size = 4; break; // mov al, moffs
    case 0xA1: case 0xA3: imm_size = 4; break; // mov eax, moffs
    case 0xA8: imm_size = 1; break; // test al, imm8
    case 0xA9: imm_size = 4; break; // test eax, imm32
    // mov r8, imm8
    case 0xB0: case 0xB1: case 0xB2: case 0xB3:
    case 0xB4: case 0xB5: case 0xB6: case 0xB7:
        imm_size = 1; break;
    // mov r32, imm32
    case 0xB8: case 0xB9: case 0xBA: case 0xBB:
    case 0xBC: case 0xBD: case 0xBE: case 0xBF:
        imm_size = 4; break;
    case 0xC0: case 0xC1: has_modrm = true; imm_size = 1; break; // shift r/m, imm8
    case 0xC2: imm_size = 2; break; // ret imm16
    case 0xC3: break; // ret
    case 0xC6: has_modrm = true; imm_size = 1; break; // mov r/m8, imm8
    case 0xC7: has_modrm = true; imm_size = 4; break; // mov r/m32, imm32
    case 0xC9: break; // leave
    case 0xCC: break; // int3
    case 0xD0: case 0xD1: case 0xD2: case 0xD3:
        has_modrm = true; break; // shift r/m
    case 0xE8: imm_size = 4; break; // call rel32
    case 0xE9: imm_size = 4; break; // jmp rel32
    case 0xEB: imm_size = 1; break; // jmp rel8
    case 0xF6: has_modrm = true;
        if (pos < maxLen && (data[pos] & 0x38) <= 0x08) imm_size = 1;
        break;
    case 0xF7: has_modrm = true;
        if (pos < maxLen && (data[pos] & 0x38) <= 0x08) imm_size = 4;
        break;
    case 0xFE: has_modrm = true; break; // inc/dec r/m8
    case 0xFF: has_modrm = true; break; // Group 5

    case 0x0F: // Two-byte opcodes
        if (pos < maxLen) {
            uint8_t op2 = data[pos++];
            if (op2 >= 0x80 && op2 <= 0x8F)      { imm_size = 4; }        // near Jcc
            else if (op2 >= 0x90 && op2 <= 0x9F)  { has_modrm = true; }   // setcc
            else if (op2 >= 0x40 && op2 <= 0x4F)  { has_modrm = true; }   // cmovcc
            else if (op2 == 0xAF)                  { has_modrm = true; }   // imul
            else if (op2 == 0xB6 || op2 == 0xB7)  { has_modrm = true; }   // movzx
            else if (op2 == 0xBE || op2 == 0xBF)  { has_modrm = true; }   // movsx
            else { return 1; } // unknown 2-byte, bail
        }
        break;

    default: return 1;
    }

    // Parse ModRM
    if (has_modrm && pos < maxLen) {
        uint8_t modrm = data[pos++];
        uint8_t mod = modrm >> 6;
        uint8_t rm = modrm & 7;
        if (mod != 3) { // memory operand
            if (rm == 4 && pos < maxLen) { // SIB
                uint8_t sib = data[pos++];
                if (mod == 0 && (sib & 7) == 5) pos += 4; // disp32
            }
            if (mod == 0 && rm == 5) pos += 4;      // [disp32]
            else if (mod == 1)       pos += 1;      // [reg+disp8]
            else if (mod == 2)       pos += 4;      // [reg+disp32]
        }
    }

    pos += imm_size;
    return (pos > 0) ? pos : 1;
}

// Check if a je instruction exists at the given offset
bool IsJeAt(const uint8_t* data, size_t dataSize, size_t off)
{
    if (off >= dataSize) return false;
    if (data[off] == 0x74) return true;
    if (data[off] == 0x0F && off + 1 < dataSize && data[off + 1] == 0x84) return true;
    return false;
}

// ---------------------------------------------------------------------------
// Extract check type IDs from a dispatch chain starting at startOff.
// Proven algorithm ported from Python/capstone analysis (extract_types_v2.py).
//
// Handles: cmp+je, cmp+jg/jge (binary split), cmp+jle/jl, cmp+jne (last type),
// sub+je/jne chains, dec+je/jne, test eax,eax + je, mov reg,imm tracking,
// cmp eax,reg (register-indirect comparison), unconditional jmp following.
// ---------------------------------------------------------------------------
void ExtractDispatchChainTypes(const uint8_t* data, size_t dataSize,
                                size_t startOff,
                                const uint8_t preRegVals[8],
                                const bool preRegValid[8],
                                std::unordered_set<uint8_t>& out)
{
    struct Branch {
        size_t start;
        uint32_t acc;
    };
    std::vector<Branch> queue;
    std::unordered_set<size_t> visited;

    queue.push_back({startOff, 0});

    while (!queue.empty() && visited.size() < kMaxBranches) {
        Branch br = queue.back();
        queue.pop_back();

        if (visited.count(br.start)) continue;
        visited.insert(br.start);

        uint32_t acc = br.acc;

        // Register tracking: EAX=0, ECX=1, EDX=2, EBX=3, ESP=4, EBP=5, ESI=6, EDI=7
        uint8_t regVals[8] = {};
        bool regValid[8] = {};
        if (preRegVals && preRegValid) {
            std::memcpy(regVals, preRegVals, 8);
            std::memcpy(regValid, preRegValid, 8);
        }

        size_t pos = br.start;
        size_t scanEnd = std::min(br.start + kChainScanLen, dataSize);

        while (pos + 1 < scanEnd) {
            uint8_t b0 = data[pos];
            bool handled = false;

            // === MOV r32, imm32 (B8+r imm32) — register tracking ===
            if (b0 >= 0xB8 && b0 <= 0xBF && pos + 4 < scanEnd) {
                uint8_t reg = b0 - 0xB8;
                uint32_t imm;
                std::memcpy(&imm, data + pos + 1, 4);
                if (imm <= 0xFF) {
                    regVals[reg] = static_cast<uint8_t>(imm);
                    regValid[reg] = true;
                }
                pos += 5;
                continue;
            }

            // === MOVZX r32, r8 (0F B6 C0-FF) — skip ===
            if (b0 == 0x0F && pos + 2 < scanEnd && data[pos + 1] == 0xB6 &&
                data[pos + 2] >= 0xC0) {
                pos += 3;
                continue;
            }

            // === CMP r32, imm8 (83 F8-FF XX) ===
            if (b0 == 0x83 && pos + 2 < scanEnd &&
                data[pos + 1] >= 0xF8 && data[pos + 1] <= 0xFF) {
                // Check if this is actually a cmp (not sub: E8-EF)
                uint8_t modrm = data[pos + 1];
                if ((modrm & 0x38) == 0x38) { // /7 = cmp
                    uint8_t imm = data[pos + 2];
                    CondJumpKind jk;
                    size_t joff = FindCondJump(data, dataSize, pos + 3, kMaxJumpDist, &jk);
                    if (joff != kNoMatch) {
                        size_t afterJcc = joff + CondJumpLen(data, joff);
                        if (jk == CJ_EQUAL) {
                            out.insert(imm);
                            pos = afterJcc;
                            handled = true;
                        } else if (jk == CJ_NOT_EQUAL) {
                            out.insert(imm);
                            size_t tgt = ComputeJumpTarget(data, dataSize, joff);
                            if (tgt != kNoMatch)
                                queue.push_back({tgt, acc});
                            break; // fall-through is handler
                        } else if (IsGreaterFamily(jk)) {
                            out.insert(imm);
                            size_t tgt = ComputeJumpTarget(data, dataSize, joff);
                            if (tgt != kNoMatch)
                                queue.push_back({tgt, 0});
                            if (IsJeAt(data, dataSize, afterJcc))
                                afterJcc += (data[afterJcc] == 0x0F) ? 6 : 2;
                            pos = afterJcc;
                            handled = true;
                        } else if (IsLessOrEq(jk)) {
                            out.insert(imm);
                            size_t tgt = ComputeJumpTarget(data, dataSize, joff);
                            if (tgt != kNoMatch)
                                queue.push_back({tgt, 0});
                            pos = afterJcc;
                            handled = true;
                        } else if (IsStrictLess(jk)) {
                            out.insert(imm);
                            size_t tgt = ComputeJumpTarget(data, dataSize, joff);
                            if (tgt != kNoMatch)
                                queue.push_back({tgt, 0});
                            if (IsJeAt(data, dataSize, afterJcc))
                                afterJcc += (data[afterJcc] == 0x0F) ? 6 : 2;
                            pos = afterJcc;
                            handled = true;
                        }
                    }
                }
            }

            // === CMP eax, imm32 (3D XX 00 00 00) ===
            if (!handled && b0 == 0x3D && pos + 4 < scanEnd &&
                data[pos + 2] == 0 && data[pos + 3] == 0 && data[pos + 4] == 0) {
                uint8_t imm = data[pos + 1];
                CondJumpKind jk;
                size_t joff = FindCondJump(data, dataSize, pos + 5, kMaxJumpDist, &jk);
                if (joff != kNoMatch) {
                    size_t afterJcc = joff + CondJumpLen(data, joff);
                    if (jk == CJ_EQUAL) {
                        out.insert(imm);
                        pos = afterJcc;
                        handled = true;
                    } else if (jk == CJ_NOT_EQUAL) {
                        out.insert(imm);
                        size_t tgt = ComputeJumpTarget(data, dataSize, joff);
                        if (tgt != kNoMatch)
                            queue.push_back({tgt, acc});
                        break;
                    } else if (IsGreaterFamily(jk)) {
                        out.insert(imm);
                        size_t tgt = ComputeJumpTarget(data, dataSize, joff);
                        if (tgt != kNoMatch) queue.push_back({tgt, 0});
                        if (IsJeAt(data, dataSize, afterJcc))
                            afterJcc += (data[afterJcc] == 0x0F) ? 6 : 2;
                        pos = afterJcc;
                        handled = true;
                    } else if (IsLessOrEq(jk)) {
                        out.insert(imm);
                        size_t tgt = ComputeJumpTarget(data, dataSize, joff);
                        if (tgt != kNoMatch) queue.push_back({tgt, 0});
                        pos = afterJcc;
                        handled = true;
                    } else if (IsStrictLess(jk)) {
                        out.insert(imm);
                        size_t tgt = ComputeJumpTarget(data, dataSize, joff);
                        if (tgt != kNoMatch) queue.push_back({tgt, 0});
                        if (IsJeAt(data, dataSize, afterJcc))
                            afterJcc += (data[afterJcc] == 0x0F) ? 6 : 2;
                        pos = afterJcc;
                        handled = true;
                    }
                }
            }

            // === CMP al, imm8 (3C XX) ===
            if (!handled && b0 == 0x3C && pos + 1 < scanEnd) {
                uint8_t imm = data[pos + 1];
                CondJumpKind jk;
                size_t joff = FindCondJump(data, dataSize, pos + 2, kMaxJumpDist, &jk);
                if (joff != kNoMatch) {
                    size_t afterJcc = joff + CondJumpLen(data, joff);
                    if (jk == CJ_EQUAL) {
                        out.insert(imm);
                        pos = afterJcc; handled = true;
                    } else if (jk == CJ_NOT_EQUAL) {
                        out.insert(imm);
                        size_t tgt = ComputeJumpTarget(data, dataSize, joff);
                        if (tgt != kNoMatch) queue.push_back({tgt, acc});
                        break;
                    } else if (IsGreaterFamily(jk)) {
                        out.insert(imm);
                        size_t tgt = ComputeJumpTarget(data, dataSize, joff);
                        if (tgt != kNoMatch) queue.push_back({tgt, 0});
                        if (IsJeAt(data, dataSize, afterJcc))
                            afterJcc += (data[afterJcc] == 0x0F) ? 6 : 2;
                        pos = afterJcc; handled = true;
                    } else if (IsLessOrEq(jk)) {
                        out.insert(imm);
                        size_t tgt = ComputeJumpTarget(data, dataSize, joff);
                        if (tgt != kNoMatch) queue.push_back({tgt, 0});
                        pos = afterJcc; handled = true;
                    } else if (IsStrictLess(jk)) {
                        out.insert(imm);
                        size_t tgt = ComputeJumpTarget(data, dataSize, joff);
                        if (tgt != kNoMatch) queue.push_back({tgt, 0});
                        if (IsJeAt(data, dataSize, afterJcc))
                            afterJcc += (data[afterJcc] == 0x0F) ? 6 : 2;
                        pos = afterJcc; handled = true;
                    }
                }
            }

            // === CMP r32, r32 (3B C0-FF) — register-indirect comparison ===
            if (!handled && b0 == 0x3B && pos + 1 < scanEnd &&
                data[pos + 1] >= 0xC0) {
                uint8_t modrm = data[pos + 1];
                uint8_t reg2 = modrm & 7;
                if (regValid[reg2] && regVals[reg2] <= 0xFF) {
                    uint8_t imm = regVals[reg2];
                    CondJumpKind jk;
                    size_t joff = FindCondJump(data, dataSize, pos + 2, kMaxJumpDist, &jk);
                    if (joff != kNoMatch) {
                        size_t afterJcc = joff + CondJumpLen(data, joff);
                        if (jk == CJ_EQUAL) {
                            out.insert(imm); pos = afterJcc; handled = true;
                        } else if (jk == CJ_NOT_EQUAL) {
                            out.insert(imm);
                            size_t tgt = ComputeJumpTarget(data, dataSize, joff);
                            if (tgt != kNoMatch) queue.push_back({tgt, acc});
                            break;
                        } else if (IsGreaterFamily(jk)) {
                            out.insert(imm);
                            size_t tgt = ComputeJumpTarget(data, dataSize, joff);
                            if (tgt != kNoMatch) queue.push_back({tgt, 0});
                            if (IsJeAt(data, dataSize, afterJcc))
                                afterJcc += (data[afterJcc] == 0x0F) ? 6 : 2;
                            pos = afterJcc; handled = true;
                        } else if (IsLessOrEq(jk)) {
                            out.insert(imm);
                            size_t tgt = ComputeJumpTarget(data, dataSize, joff);
                            if (tgt != kNoMatch) queue.push_back({tgt, 0});
                            pos = afterJcc; handled = true;
                        } else if (IsStrictLess(jk)) {
                            out.insert(imm);
                            size_t tgt = ComputeJumpTarget(data, dataSize, joff);
                            if (tgt != kNoMatch) queue.push_back({tgt, 0});
                            if (IsJeAt(data, dataSize, afterJcc))
                                afterJcc += (data[afterJcc] == 0x0F) ? 6 : 2;
                            pos = afterJcc; handled = true;
                        }
                    }
                }
            }

            // === TEST r32, r32 (85 C0-FF with reg1==reg2) — zero check ===
            if (!handled && b0 == 0x85 && pos + 1 < scanEnd &&
                data[pos + 1] >= 0xC0) {
                uint8_t modrm = data[pos + 1];
                uint8_t reg1 = (modrm >> 3) & 7;
                uint8_t reg2 = modrm & 7;
                if (reg1 == reg2) {
                    CondJumpKind jk;
                    size_t joff = FindCondJump(data, dataSize, pos + 2, kMaxJumpDist, &jk);
                    if (joff != kNoMatch && jk == CJ_EQUAL) {
                        out.insert(static_cast<uint8_t>(acc));
                        pos = joff + CondJumpLen(data, joff);
                        handled = true;
                    }
                }
            }

            // === SUB r32, imm8 (83 E8-EF XX) ===
            if (!handled && b0 == 0x83 && pos + 2 < scanEnd &&
                data[pos + 1] >= 0xE8 && data[pos + 1] <= 0xEF) {
                uint8_t delta = data[pos + 2];
                CondJumpKind jk;
                size_t joff = FindCondJump(data, dataSize, pos + 3, kMaxJumpDist, &jk);
                if (joff != kNoMatch && (jk == CJ_EQUAL || jk == CJ_NOT_EQUAL)) {
                    acc += delta;
                    out.insert(static_cast<uint8_t>(acc));
                    if (jk == CJ_NOT_EQUAL) {
                        size_t tgt = ComputeJumpTarget(data, dataSize, joff);
                        if (tgt != kNoMatch)
                            queue.push_back({tgt, acc});
                        break; // fall-through is handler
                    }
                    pos = joff + CondJumpLen(data, joff);
                    handled = true;
                }
            }

            // === SUB al, imm8 (2C XX) ===
            if (!handled && b0 == 0x2C && pos + 1 < scanEnd) {
                CondJumpKind jk;
                size_t joff = FindCondJump(data, dataSize, pos + 2, kMaxJumpDist, &jk);
                if (joff != kNoMatch && (jk == CJ_EQUAL || jk == CJ_NOT_EQUAL)) {
                    acc += data[pos + 1];
                    out.insert(static_cast<uint8_t>(acc));
                    if (jk == CJ_NOT_EQUAL) {
                        size_t tgt = ComputeJumpTarget(data, dataSize, joff);
                        if (tgt != kNoMatch)
                            queue.push_back({tgt, acc});
                        break;
                    }
                    pos = joff + CondJumpLen(data, joff);
                    handled = true;
                }
            }

            // === SUB eax, imm32 byte-range (2D XX 00 00 00) ===
            if (!handled && b0 == 0x2D && pos + 4 < scanEnd &&
                data[pos + 2] == 0 && data[pos + 3] == 0 && data[pos + 4] == 0) {
                CondJumpKind jk;
                size_t joff = FindCondJump(data, dataSize, pos + 5, kMaxJumpDist, &jk);
                if (joff != kNoMatch && (jk == CJ_EQUAL || jk == CJ_NOT_EQUAL)) {
                    acc += data[pos + 1];
                    out.insert(static_cast<uint8_t>(acc));
                    if (jk == CJ_NOT_EQUAL) {
                        size_t tgt = ComputeJumpTarget(data, dataSize, joff);
                        if (tgt != kNoMatch)
                            queue.push_back({tgt, acc});
                        break;
                    }
                    pos = joff + CondJumpLen(data, joff);
                    handled = true;
                }
            }

            // === DEC r32 (48-4F) ===
            if (!handled && b0 >= 0x48 && b0 <= 0x4F) {
                CondJumpKind jk;
                size_t joff = FindCondJump(data, dataSize, pos + 1, kMaxJumpDist, &jk);
                if (joff != kNoMatch && (jk == CJ_EQUAL || jk == CJ_NOT_EQUAL)) {
                    acc += 1;
                    out.insert(static_cast<uint8_t>(acc));
                    if (jk == CJ_NOT_EQUAL) {
                        size_t tgt = ComputeJumpTarget(data, dataSize, joff);
                        if (tgt != kNoMatch)
                            queue.push_back({tgt, acc});
                        break;
                    }
                    pos = joff + CondJumpLen(data, joff);
                    handled = true;
                }
            }

            // === JMP rel8 (EB XX) — follow unconditional jump ===
            if (!handled && b0 == 0xEB && pos + 1 < scanEnd) {
                int8_t disp = static_cast<int8_t>(data[pos + 1]);
                intptr_t tgt = static_cast<intptr_t>(pos) + 2 + disp;
                if (tgt >= 0 && static_cast<size_t>(tgt) < dataSize)
                    queue.push_back({static_cast<size_t>(tgt), acc});
                break;
            }
            // === JMP rel32 (E9 XX XX XX XX) ===
            if (!handled && b0 == 0xE9 && pos + 4 < scanEnd) {
                int32_t disp;
                std::memcpy(&disp, data + pos + 1, 4);
                intptr_t tgt = static_cast<intptr_t>(pos) + 5 + disp;
                if (tgt >= 0 && static_cast<size_t>(tgt) < dataSize)
                    queue.push_back({static_cast<size_t>(tgt), acc});
                break;
            }

            // === Stop conditions ===
            if (!handled) {
                // push (50-57, 68, 6A, FF /6)
                if ((b0 >= 0x50 && b0 <= 0x57) || b0 == 0x68 || b0 == 0x6A)
                    break;
                // call (E8, FF /2)
                if (b0 == 0xE8) break;
                if (b0 == 0xFF && pos + 1 < scanEnd && (data[pos + 1] & 0x38) == 0x10)
                    break;
                // ret, leave, int3
                if (b0 == 0xC3 || b0 == 0xC9 || b0 == 0xCC) break;
                // lea r32, [ebp-X] or [esp+X] — stack arg setup = handler entry
                if (b0 == 0x8D && pos + 1 < scanEnd) {
                    uint8_t modrm = data[pos + 1];
                    uint8_t rm = modrm & 7;
                    uint8_t mod = modrm >> 6;
                    if (mod != 3 && (rm == 4 || rm == 5)) // esp or ebp base
                        break;
                }
            }

            // === Default: advance by instruction length ===
            if (!handled) {
                size_t len = X86InsnLen(data + pos, scanEnd - pos);
                pos += len ? len : 1;
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Remap table cross-reference: extract types from modules where ALL XOR sites
// are remap tables (no dispatch chain). Groups by index value, finds singletons
// + pairs, intersects across tables.
// ---------------------------------------------------------------------------
struct RemapTableInfo {
    uint32_t remapOff;
    uint32_t jtableOff;
    uint8_t shift;
    uint8_t maxType;
};

bool DetectRemapTableInfo(const uint8_t* data, size_t dataSize,
                           size_t afterMovzx, RemapTableInfo* out)
{
    // Scan for: [add eax, -shift / sub eax, shift] [cmp eax, max; ja] movzx byte [reg+large_disp] jmp [reg*4+disp]
    size_t end = std::min(afterMovzx + kRemapCheckDist, dataSize);
    uint8_t shift = 0;
    uint8_t maxType = 0xFF;

    // Scan for shift and maxType in the window
    for (size_t i = afterMovzx; i + 2 < end; ) {
        uint8_t b = data[i];
        // add r32, sign-extended imm8 (83 C0-C7 XX)
        if (b == 0x83 && data[i + 1] >= 0xC0 && data[i + 1] <= 0xC7) {
            uint8_t imm = data[i + 2];
            if (imm >= 0x80) // negative sign-extended
                shift = static_cast<uint8_t>(0x100 - imm);
            i += 3; continue;
        }
        // sub r32, imm8 (83 E8-EF XX)
        if (b == 0x83 && data[i + 1] >= 0xE8 && data[i + 1] <= 0xEF) {
            uint8_t imm = data[i + 2];
            if (imm > 0 && imm < 0x80) shift = imm;
            i += 3; continue;
        }
        // sub al, imm8 (2C XX)
        if (b == 0x2C) {
            uint8_t imm = data[i + 1];
            if (imm > 0 && imm < 0x80) shift = imm;
            i += 2; continue;
        }
        // cmp r32, imm8 (83 F8-FF XX) — maxType
        if (b == 0x83 && data[i + 1] >= 0xF8 && data[i + 1] <= 0xFF) {
            uint8_t imm = data[i + 2];
            if (imm >= 0x80) maxType = imm;
            i += 3; continue;
        }
        // cmp al, imm8 (3C XX) — maxType
        if (b == 0x3C) {
            uint8_t imm = data[i + 1];
            if (imm >= 0x80) maxType = imm;
            i += 2; continue;
        }
        // cmp eax, imm32 (3D XX 00 00 00) — maxType
        if (b == 0x3D && i + 4 < end && data[i + 2] == 0 && data[i + 3] == 0 && data[i + 4] == 0) {
            uint8_t imm = data[i + 1];
            if (imm >= 0x80) maxType = imm;
            i += 5; continue;
        }
        size_t len = X86InsnLen(data + i, end - i);
        i += len ? len : 1;
    }

    // Scan for movzx byte ptr [reg+large_disp] followed by jmp [reg*4+disp]
    for (size_t j = afterMovzx; j + 6 < end; ++j) {
        if (data[j] != 0x0F || data[j + 1] != 0xB6) continue;
        uint8_t m = data[j + 2];
        if (m < 0x80 || m > 0xBF || (m & 7) == 4) continue; // need mod=10 (disp32), no SIB
        uint32_t disp;
        std::memcpy(&disp, data + j + 3, 4);
        // Adjust absolute displacement for in-memory (relocated) modules
        if (g_scanBaseAddr != 0 && disp >= g_scanBaseAddr)
            disp -= static_cast<uint32_t>(g_scanBaseAddr);
        if (disp <= 0x100) continue;
        if (disp < dataSize && disp + 256 > dataSize) continue;
        // Look for jmp [reg*4 + disp] nearby
        for (size_t k = j + 7; k + 6 < dataSize && k < j + 25; ++k) {
            if (data[k] == 0xFF && data[k + 1] == 0x24) {
                uint8_t sib = data[k + 2];
                if ((sib >> 6) == 2 && (sib & 7) == 5) { // scale=4, base=disp32
                    uint32_t jtDisp;
                    std::memcpy(&jtDisp, data + k + 3, 4);
                    if (g_scanBaseAddr != 0 && jtDisp >= g_scanBaseAddr)
                        jtDisp -= static_cast<uint32_t>(g_scanBaseAddr);
                    if (out) {
                        out->remapOff = disp;
                        out->jtableOff = jtDisp;
                        out->shift = shift;
                        out->maxType = maxType;
                    }
                    return true;
                }
            }
        }
    }
    return false;
}

bool ExtractFromRemapCrossRef(const uint8_t* data, size_t dataSize,
                                const std::vector<RemapTableInfo>& remaps,
                                std::unordered_set<uint8_t>& outTypes)
{
    if (remaps.size() < 2)
        return false;

    // For each remap table, compute singletons + pairs (grouped by index value)
    std::vector<std::unordered_set<uint8_t>> spSets;

    for (const auto& r : remaps) {
        if (r.remapOff + r.maxType + 1 > dataSize)
            continue;

        const uint8_t* table = data + r.remapOff;
        size_t tableLen = static_cast<size_t>(r.maxType) + 1;

        // Compute max valid handler index from jump table size
        int maxHandler = -1;
        if (r.jtableOff < r.remapOff) {
            uint32_t gap = r.remapOff - r.jtableOff;
            if (gap >= 4 && gap % 4 == 0)
                maxHandler = static_cast<int>(gap / 4 - 1);
        }

        // Count occurrences of each index byte (only valid handler indices)
        uint16_t indexCounts[256] = {};
        for (size_t i = 0; i < tableLen; ++i) {
            uint8_t idx = table[i];
            if (maxHandler >= 0 && idx > maxHandler) continue;
            indexCounts[idx]++;
        }

        // Default = most common index
        uint8_t defaultIdx = 0;
        uint16_t maxCount = 0;
        for (int v = 0; v < 256; ++v) {
            if (indexCounts[v] > maxCount) {
                maxCount = indexCounts[v];
                defaultIdx = static_cast<uint8_t>(v);
            }
        }

        // Group type bytes by their index value — skip entries with invalid handler index
        // groups[idx] = list of raw type offsets
        std::unordered_map<uint8_t, std::vector<uint8_t>> groups;
        for (size_t i = 0; i < tableLen; ++i) {
            uint8_t idx = table[i];
            if (idx == defaultIdx) continue;
            if (maxHandler >= 0 && idx > maxHandler) continue;
            groups[idx].push_back(static_cast<uint8_t>(i));
        }

        // Collect singletons + pairs + triplets with shift applied
        std::unordered_set<uint8_t> sp;
        for (const auto& entry : groups) {
            if (entry.second.size() <= 3) {
                for (uint8_t raw : entry.second)
                    sp.insert(static_cast<uint8_t>((raw + r.shift) & 0xFF));
            }
        }
        spSets.push_back(sp);

        LOG(INFO) << "[WARDEN_SCAN]   Remap @ 0x" << std::hex << r.remapOff
                  << " (shift=0x" << static_cast<int>(r.shift)
                  << ", max=0x" << static_cast<int>(r.maxType) << "): "
                  << std::dec << sp.size() << " singleton/pair/triplet candidates";
    }

    if (spSets.size() < 2)
        return false;

    // Cross-reference: intersection of singletons+pairs across all tables
    std::unordered_set<uint8_t> intersection = spSets[0];
    for (size_t i = 1; i < spSets.size(); ++i) {
        std::unordered_set<uint8_t> tmp;
        for (uint8_t t : intersection) {
            if (spSets[i].count(t))
                tmp.insert(t);
        }
        intersection = tmp;
    }

    if (intersection.size() >= kMinChainTypes) {
        outTypes = intersection;
        return true;
    }
    return false;
}

// Extract type IDs from a single remap table by finding all non-default entries.
// The default handler index is the most common value in the table.
bool ExtractFromSingleRemap(const uint8_t* data, size_t dataSize,
                             const RemapTableInfo& r,
                             std::unordered_set<uint8_t>& outTypes)
{
    size_t tableLen = static_cast<size_t>(r.maxType) + 1;
    if (r.remapOff + tableLen > dataSize)
        return false;

    const uint8_t* table = data + r.remapOff;

    // Compute max valid handler index from jump table size
    int maxHandler = -1;
    if (r.jtableOff < r.remapOff) {
        uint32_t gap = r.remapOff - r.jtableOff;
        if (gap >= 4 && gap % 4 == 0)
            maxHandler = static_cast<int>(gap / 4 - 1);
    }

    // Find default handler index (most common value, only valid handler indices)
    uint16_t indexCounts[256] = {};
    for (size_t i = 0; i < tableLen; ++i) {
        uint8_t idx = table[i];
        if (maxHandler >= 0 && idx > maxHandler) continue;
        indexCounts[idx]++;
    }

    uint8_t defaultIdx = 0;
    uint16_t maxCount = 0;
    for (int v = 0; v < 256; ++v) {
        if (indexCounts[v] > maxCount) {
            maxCount = indexCounts[v];
            defaultIdx = static_cast<uint8_t>(v);
        }
    }

    // All entries that differ from the default are real type IDs (skip invalid handler indices)
    std::unordered_set<uint8_t> types;
    for (size_t i = 0; i < tableLen; ++i) {
        if (table[i] != defaultIdx && (maxHandler < 0 || table[i] <= maxHandler))
            types.insert(static_cast<uint8_t>((i + r.shift) & 0xFF));
    }

    if (types.size() >= kMinChainTypes) {
        outTypes = types;

        std::vector<uint8_t> sorted(types.begin(), types.end());
        std::sort(sorted.begin(), sorted.end());
        std::ostringstream oss;
        oss << "[WARDEN_SCAN] Single remap table @ 0x" << std::hex << r.remapOff
            << " (shift=0x" << static_cast<int>(r.shift)
            << ", max=0x" << static_cast<int>(r.maxType)
            << ", default=0x" << static_cast<int>(defaultIdx)
            << "): " << std::dec << types.size() << " types:";
        for (uint8_t id : sorted)
            oss << " 0x" << std::hex << std::setfill('0') << std::setw(2)
                << static_cast<int>(id);
        LOG(INFO) << oss.str();
        return true;
    }
    return false;
}

// Fix maxType for remap tables where the CMP immediate was < 0x80 and got missed
// by DetectRemapTableInfo. Backward-scans from the movzx remap reference to find
// the last CMP + JA/JAE pair.
uint8_t FixMaxType(const uint8_t* data, size_t dataSize, const RemapTableInfo& r)
{
    if (r.maxType != 0xFF) return r.maxType;  // already valid

    // Step 1: Find movzx byte [reg + remapOff] reference in code
    // Pattern: 0F B6 [80-BF, rm!=4] [LE32 disp]
    size_t codeAddr = 0;
    bool found = false;
    for (size_t i = 0; i + 6 < dataSize; ++i) {
        if (data[i] == 0x0F && data[i + 1] == 0xB6
            && data[i + 2] >= 0x80 && data[i + 2] <= 0xBF && (data[i + 2] & 7) != 4) {
            uint32_t disp;
            std::memcpy(&disp, data + i + 3, 4);
            if (g_scanBaseAddr != 0 && disp >= g_scanBaseAddr)
                disp -= static_cast<uint32_t>(g_scanBaseAddr);
            if (disp == r.remapOff) {
                codeAddr = i;
                found = true;
                break;
            }
        }
    }
    if (!found) return 0xFF;

    // Step 2: Forward-disassemble from (codeAddr - 60) to codeAddr,
    // tracking the LAST CMP + JA/JAE pair
    size_t start = (codeAddr > 60) ? codeAddr - 60 : 0;
    uint8_t bestCmp = 0xFF;
    uint8_t lastCmpImm = 0;
    bool hasCmp = false;

    for (size_t pos = start; pos < codeAddr; ) {
        uint8_t b = data[pos];
        // CMP r32, imm8 (83 F8-FF XX)
        if (b == 0x83 && pos + 2 < dataSize && data[pos + 1] >= 0xF8 && data[pos + 1] <= 0xFF) {
            lastCmpImm = data[pos + 2]; hasCmp = (lastCmpImm > 0);
            pos += 3; continue;
        }
        // CMP al, imm8 (3C XX)
        if (b == 0x3C && pos + 1 < dataSize) {
            lastCmpImm = data[pos + 1]; hasCmp = (lastCmpImm > 0);
            pos += 2; continue;
        }
        // CMP eax, imm32 (3D XX 00 00 00)
        if (b == 0x3D && pos + 4 < dataSize
            && data[pos + 2] == 0 && data[pos + 3] == 0 && data[pos + 4] == 0) {
            lastCmpImm = data[pos + 1]; hasCmp = (lastCmpImm > 0);
            pos += 5; continue;
        }
        // JA short (77 XX) or JA near (0F 87 XX XX XX XX)
        if (hasCmp && (b == 0x77 || (b == 0x0F && pos + 1 < dataSize && data[pos + 1] == 0x87))) {
            bestCmp = lastCmpImm;
        }
        // JAE short (73 XX) or JAE near (0F 83 XX XX XX XX)
        if (hasCmp && (b == 0x73 || (b == 0x0F && pos + 1 < dataSize && data[pos + 1] == 0x83))) {
            bestCmp = lastCmpImm;
        }
        size_t len = X86InsnLen(data + pos, codeAddr - pos);
        pos += len ? len : 1;
    }

    if (bestCmp != 0xFF) {
        LOG(INFO) << "[WARDEN_SCAN] FixMaxType: remap @ 0x" << std::hex << r.remapOff
                  << " maxType fixed 0xFF -> 0x" << static_cast<int>(bestCmp);
    }
    return bestCmp;
}

// Helper: pre-scan XOR-to-MOVZX window for register loads, then run chain walker.
void PreScanAndExtractChainTypes(const uint8_t* data, size_t size,
                                   size_t xorOff, size_t movzxOff, size_t afterMovzx,
                                   std::unordered_set<uint8_t>& out)
{
    uint8_t preRegVals[8] = {};
    bool preRegValid[8] = {};
    for (size_t p = xorOff; p + 4 < movzxOff; ) {
        uint8_t b = data[p];
        if (b >= 0xB8 && b <= 0xBF && p + 4 < size) {
            uint8_t reg = b - 0xB8;
            uint32_t imm;
            std::memcpy(&imm, data + p + 1, 4);
            if (imm <= 0xFF) {
                preRegVals[reg] = static_cast<uint8_t>(imm);
                preRegValid[reg] = true;
            }
            p += 5; continue;
        }
        size_t len = X86InsnLen(data + p, movzxOff - p);
        p += len ? len : 1;
    }
    ExtractDispatchChainTypes(data, size, afterMovzx, preRegVals, preRegValid, out);
}

// Main XOR-anchored scanner: find XOR sites, classify, extract types.
// Two strategies: dispatch chain extraction (primary) and remap cross-reference (fallback).
bool ScanForDispatchChainInBinary(const uint8_t* data, size_t size, size_t scanStart)
{
    // Step 1: Find all XOR r8, [reg+4] + movzx r32, r8 sites
    struct XorSite { size_t xorOff; size_t movzxOff; size_t afterMovzx; };
    std::vector<XorSite> sites;

    for (size_t i = scanStart; i + 2 < size; ++i) {
        if (data[i] != 0x32) continue;
        uint8_t modrm = data[i + 1];
        if (modrm < 0x40 || modrm > 0x7F) continue;
        if ((modrm & 7) == 4) continue;
        if (data[i + 2] != 0x04) continue;

        for (size_t j = i + 3; j + 2 < size && j < i + 3 + kMaxMovzxDist; ++j) {
            if (data[j] == 0x0F && data[j + 1] == 0xB6 && data[j + 2] >= 0xC0) {
                sites.push_back({i, j, j + 3});
                break;
            }
        }
    }

    if (sites.empty())
        return false;

    LOG(INFO) << "[WARDEN_SCAN] XOR-anchored scan: " << sites.size()
              << " XOR+MOVZX site(s) in module binary";

    // Step 2: Classify each site and extract types
    // Union all dispatch chain type sets (different chains may cover different types)
    std::unordered_set<uint8_t> allChainTypes;
    size_t bestXorOff = 0;
    size_t bestChainSize = 0;
    int chainCount = 0;
    std::vector<RemapTableInfo> remapInfos;

    for (const auto& site : sites) {
        // Check for remap table and extract parameters
        RemapTableInfo rinfo = {};
        bool isRemap = DetectRemapTableInfo(data, size, site.afterMovzx, &rinfo);

        if (isRemap) {
            remapInfos.push_back(rinfo);
            LOG(INFO) << "[WARDEN_SCAN]   XOR @ 0x" << std::hex << std::setfill('0')
                      << std::setw(4) << site.xorOff
                      << ": remap table (remap=0x" << std::setw(4) << rinfo.remapOff
                      << ", shift=0x" << std::setw(2) << static_cast<int>(rinfo.shift)
                      << ", max=0x" << std::setw(2) << static_cast<int>(rinfo.maxType) << ")";
            continue;
        }

        // Extract dispatch chain types (pre-scan + walker)
        std::unordered_set<uint8_t> types;
        PreScanAndExtractChainTypes(data, size, site.xorOff,
                                     site.movzxOff, site.afterMovzx, types);

        if (!types.empty()) {
            std::vector<uint8_t> sorted(types.begin(), types.end());
            std::sort(sorted.begin(), sorted.end());
            std::ostringstream oss;
            oss << "[WARDEN_SCAN]   XOR @ 0x" << std::hex << std::setfill('0')
                << std::setw(4) << site.xorOff << ": dispatch chain, "
                << std::dec << types.size() << " types:";
            for (uint8_t id : sorted)
                oss << " 0x" << std::hex << std::setfill('0') << std::setw(2)
                    << static_cast<int>(id);
            LOG(INFO) << oss.str();

            for (uint8_t t : types)
                allChainTypes.insert(t);
            chainCount++;
            if (types.size() > bestChainSize) {
                bestChainSize = types.size();
                bestXorOff = site.xorOff;
            }
        }
    }

    // Step 3: If dispatch chain(s) found, start with chain types.
    // Then supplement from remap table if available (chain may miss some BST branches).
    if (allChainTypes.size() >= kMinChainTypes) {
        std::vector<uint8_t> sorted(allChainTypes.begin(), allChainTypes.end());
        std::sort(sorted.begin(), sorted.end());
        std::ostringstream oss;
        oss << "[WARDEN_SCAN] Union of " << std::dec << chainCount
            << " dispatch chain(s) (best XOR @ 0x"
            << std::hex << std::uppercase << std::setfill('0')
            << std::setw(4) << bestXorOff
            << "): " << std::dec << sorted.size() << " check type IDs:";
        for (uint8_t id : sorted)
            oss << " 0x" << std::hex << std::setfill('0') << std::setw(2)
                << static_cast<int>(id);
        LOG(INFO) << oss.str();

        // Try to supplement from remap table (authoritative — contains ALL types)
        std::unordered_set<uint8_t> remapTypes;
        bool remapOk = false;
        if (remapInfos.size() >= 2)
            remapOk = ExtractFromRemapCrossRef(data, size, remapInfos, remapTypes);
        if (!remapOk) {
            // Try each table with maxType fixup, pick table closest to 9 entries
            for (const auto& r : remapInfos) {
                RemapTableInfo fixed = r;
                fixed.maxType = FixMaxType(data, size, r);
                std::unordered_set<uint8_t> candidate;
                if (ExtractFromSingleRemap(data, size, fixed, candidate)) {
                    if (candidate.size() >= 9 && candidate.size() <= 10) {
                        remapTypes = candidate;
                        remapOk = true;
                        break;  // perfect match
                    }
                    if (!remapOk ||
                        std::abs(static_cast<int>(candidate.size()) - 9) <
                        std::abs(static_cast<int>(remapTypes.size()) - 9)) {
                        remapTypes = candidate;
                        remapOk = true;
                    }
                }
            }
        }

        if (remapOk && remapTypes.size() > allChainTypes.size() && remapTypes.size() <= 12) {
            // Remap table found more types — use it as the definitive set
            g_typeIDs = remapTypes;
            LOG(INFO) << "[WARDEN_SCAN] Remap table supplemented chain: "
                      << allChainTypes.size() << " -> " << remapTypes.size() << " types";
        } else {
            g_typeIDs = allChainTypes;
        }

        g_typeSizes.clear();
        for (uint8_t id : g_typeIDs)
            g_typeSizes[id] = -1;
        g_hasTypeIDs = true;
        g_allSizesKnown = false;
        return true;
    }

    // Step 4: No dispatch chain found — try remap table extraction
    {
        std::unordered_set<uint8_t> remapTypes;
        bool remapOk = false;
        if (remapInfos.size() >= 2) {
            LOG(INFO) << "[WARDEN_SCAN] No dispatch chain found, trying remap cross-reference ("
                      << remapInfos.size() << " remap tables)...";
            remapOk = ExtractFromRemapCrossRef(data, size, remapInfos, remapTypes);
        }
        if (!remapOk) {
            // Try each table with maxType fixup, pick table closest to 9 entries
            for (const auto& r : remapInfos) {
                RemapTableInfo fixed = r;
                fixed.maxType = FixMaxType(data, size, r);
                std::unordered_set<uint8_t> candidate;
                if (ExtractFromSingleRemap(data, size, fixed, candidate)) {
                    if (candidate.size() >= 9 && candidate.size() <= 10) {
                        remapTypes = candidate;
                        remapOk = true;
                        break;  // perfect match
                    }
                    if (!remapOk ||
                        std::abs(static_cast<int>(candidate.size()) - 9) <
                        std::abs(static_cast<int>(remapTypes.size()) - 9)) {
                        remapTypes = candidate;
                        remapOk = true;
                    }
                }
            }
        }
        if (remapOk) {
            g_typeIDs = remapTypes;
            g_typeSizes.clear();
            for (uint8_t id : g_typeIDs)
                g_typeSizes[id] = -1;
            g_hasTypeIDs = true;
            g_allSizesKnown = false;

            std::vector<uint8_t> sorted(g_typeIDs.begin(), g_typeIDs.end());
            std::sort(sorted.begin(), sorted.end());
            std::ostringstream oss;
            oss << "[WARDEN_SCAN] Remap table extraction: " << std::dec << sorted.size()
                << " check type IDs:";
            for (uint8_t id : sorted)
                oss << " 0x" << std::hex << std::setfill('0') << std::setw(2)
                    << static_cast<int>(id);
            LOG(INFO) << oss.str();
            return true;
        }
    }

    // Step 5: Last resort — try chain extraction on remap-classified sites
    // Some sites may be misclassified as remap when they're actually dispatch chains
    if (allChainTypes.size() < kMinChainTypes && !remapInfos.empty()) {
        LOG(INFO) << "[WARDEN_SCAN] Trying chain extraction on "
                  << remapInfos.size() << " remap-classified site(s) as last resort...";

        for (const auto& site : sites) {
            std::unordered_set<uint8_t> types;
            PreScanAndExtractChainTypes(data, size, site.xorOff,
                                         site.movzxOff, site.afterMovzx, types);
            for (uint8_t t : types)
                allChainTypes.insert(t);
        }

        if (allChainTypes.size() >= kMinChainTypes) {
            g_typeIDs = allChainTypes;
            g_typeSizes.clear();
            for (uint8_t id : g_typeIDs)
                g_typeSizes[id] = -1;
            g_hasTypeIDs = true;
            g_allSizesKnown = false;

            std::vector<uint8_t> sorted(g_typeIDs.begin(), g_typeIDs.end());
            std::sort(sorted.begin(), sorted.end());
            std::ostringstream oss;
            oss << "[WARDEN_SCAN] Last-resort chain extraction: "
                << std::dec << sorted.size() << " check type IDs:";
            for (uint8_t id : sorted)
                oss << " 0x" << std::hex << std::setfill('0') << std::setw(2)
                    << static_cast<int>(id);
            LOG(INFO) << oss.str();
            return true;
        }
    }

    if (!allChainTypes.empty())
        LOG(INFO) << "[WARDEN_SCAN] Dispatch chain union had only "
                  << allChainTypes.size() << " types (need " << kMinChainTypes << ")";
    else
        LOG(INFO) << "[WARDEN_SCAN] No dispatch chain types found in module binary";

    return false;
}

// ---------------------------------------------------------------------------
// Scan a memory region for the Warden dispatcher pattern.
// Precondition: caller must filter for MEM_PRIVATE regions only.
// MEM_IMAGE (WoW.exe, DLLs) causes false positives from normal x86 code.
// ---------------------------------------------------------------------------
bool ScanRegionForDispatcher(uintptr_t baseAddr, size_t regionSize)
{
    if (regionSize > kMaxRegionSize || regionSize < kDispatcherWindow)
        return false;

    // Copy region into local buffer for safe scanning
    std::vector<uint8_t> buf(regionSize);
    if (!SafeMemcpy(buf.data(), reinterpret_cast<const void*>(baseAddr), regionSize))
        return false;

    // Primary: XOR-anchored dispatch chain scanner (works on in-memory code too)
    // Set base address so absolute displacements in relocated code get adjusted
    g_scanBaseAddr = baseAddr;
    if (ScanForDispatchChainInBinary(buf.data(), regionSize, 0)) {
        LOG(INFO) << "[WARDEN_SCAN] (found in memory region 0x"
                  << std::hex << std::uppercase << std::setfill('0')
                  << std::setw(8) << baseAddr
                  << ", " << std::dec << regionSize << " bytes)";

        // Store runtime address if not already set by FindModuleInMemory
        if (g_moduleRuntimeBase == 0) {
            MEMORY_BASIC_INFORMATION mbi2;
            if (VirtualQuery(reinterpret_cast<LPCVOID>(baseAddr), &mbi2, sizeof(mbi2))
                == sizeof(mbi2))
            {
                g_moduleRuntimeBase = reinterpret_cast<uintptr_t>(mbi2.AllocationBase);
                g_moduleRuntimeSize = regionSize;
                LOG(INFO) << "[WARDEN_SCAN] Saved runtime base 0x"
                          << std::hex << std::uppercase << std::setfill('0')
                          << std::setw(8) << g_moduleRuntimeBase
                          << " from blind scan";
            }
        }

        return true;
    }

    // Reset base address if XOR-anchored scan failed (avoid stale state for next region)
    g_scanBaseAddr = 0;

    // Fallback: cmp-cluster scanner
    size_t offset = 0;
    if (ScanBufferForDispatcher(buf.data(), regionSize,
                                kDispatcherWindow, kMinUniqueTypes, &offset)) {
        std::vector<uint8_t> sorted(g_typeIDs.begin(), g_typeIDs.end());
        std::sort(sorted.begin(), sorted.end());

        std::ostringstream oss;
        oss << "[WARDEN_SCAN] Found cmp-cluster in memory at region 0x"
            << std::hex << std::uppercase << std::setfill('0')
            << std::setw(8) << baseAddr
            << "+0x" << std::setw(4) << offset
            << ", extracted " << std::dec << sorted.size() << " check type IDs:";
        for (uint8_t id : sorted)
            oss << " 0x" << std::hex << std::setfill('0') << std::setw(2)
                << static_cast<int>(id);
        LOG(INFO) << oss.str();

        // Store runtime address if not already set by FindModuleInMemory
        if (g_moduleRuntimeBase == 0) {
            MEMORY_BASIC_INFORMATION mbi2;
            if (VirtualQuery(reinterpret_cast<LPCVOID>(baseAddr), &mbi2, sizeof(mbi2))
                == sizeof(mbi2))
            {
                g_moduleRuntimeBase = reinterpret_cast<uintptr_t>(mbi2.AllocationBase);
                g_moduleRuntimeSize = regionSize;
                LOG(INFO) << "[WARDEN_SCAN] Saved runtime base 0x"
                          << std::hex << std::uppercase << std::setfill('0')
                          << std::setw(8) << g_moduleRuntimeBase
                          << " from blind scan (cmp-cluster)";
            }
        }

        return true;
    }

    return false;
}

// ---------------------------------------------------------------------------
// Deterministic size assignment: structural validation for a single check.
//
// For a new (unknown-size) type at position pos, try each candidate size
// in order [31, 29, 25, 24, 6, 1, 0] and validate structural properties
// of the data bytes. Returns the matching size, or -1 if none fits.
//
// Data bytes are plaintext (not XOR'd) — only type bytes use xorByte.
// ---------------------------------------------------------------------------
bool IsValidAddress(uint32_t addr)
{
    // Lower bound 0x1000: some PAGE checks target low addresses (e.g., 0xA088, 0x74BC)
    return addr >= 0x1000 && addr <= 0x7FFFFFFF;
}

int TryAssignSize(const uint8_t* data, size_t pos, size_t checkEnd,
                  size_t numStrings, uint8_t xorByte)
{
    int bestSize = -1;
    int matchCount = 0;

    for (size_t c = 0; c < kNumKnownSizes; ++c) {
        int candidateSize = kKnownSizes[c];
        if (pos + candidateSize > checkEnd)
            continue;

        bool valid = false;

        switch (candidateSize) {
        case 31: {
            // PROC: seed(4)+SHA1(20)+modIdx(1)+procIdx(1)+addr(4)+readLen(1)
            uint8_t modIdx  = data[pos + 24];
            uint8_t procIdx = data[pos + 25];
            uint32_t addr;
            std::memcpy(&addr, data + pos + 26, 4);
            uint8_t readLen = data[pos + 30];
            valid = (modIdx <= numStrings && procIdx <= numStrings &&
                     IsValidAddress(addr) && readLen >= 1 && readLen <= 64);
            break;
        }
        case 29: {
            // PAGE: seed(4)+SHA1(20)+addr(4)+readLen(1)
            uint32_t addr;
            std::memcpy(&addr, data + pos + 24, 4);
            uint8_t readLen = data[pos + 28];
            valid = (IsValidAddress(addr) && readLen >= 1 && readLen <= 64);
            break;
        }
        case 25: {
            // DRIVER: seed(4)+SHA1(20)+stringIndex(1)
            uint8_t strIdx = data[pos + 24];
            valid = (strIdx <= numStrings);
            break;
        }
        case 24: {
            // MODULE: seed(4)+SHA1(20) — no trailing field to validate
            // Look-ahead: next byte must be a known type or end-of-section
            size_t nextPos = pos + 24;
            if (nextPos == checkEnd) {
                valid = true;
            } else if (nextPos < checkEnd) {
                uint8_t nextType = data[nextPos] ^ xorByte;
                valid = g_typeIDs.count(nextType) > 0;
            }
            break;
        }
        case 6: {
            // MEM: unk(1)+addr(4)+readLen(1)
            // NOTE: unk byte is assumed 0x00 (observed in all captured packets).
            // If unk != 0x00, this size won't match — safe false negative.
            uint32_t addr;
            std::memcpy(&addr, data + pos + 1, 4);
            uint8_t readLen = data[pos + 5];
            valid = (data[pos] == 0x00 && IsValidAddress(addr) &&
                     readLen >= 1 && readLen <= 64);
            break;
        }
        case 1: {
            // MPQ/LUA: stringIndex(1)
            uint8_t strIdx = data[pos];
            valid = (strIdx <= numStrings);
            break;
        }
        case 0: {
            // TIMING: empty — look-ahead: next byte must be known type or end
            if (pos == checkEnd) {
                valid = true;
            } else {
                uint8_t nextType = data[pos] ^ xorByte;
                valid = g_typeIDs.count(nextType) > 0;
            }
            break;
        }
        }

        if (valid) {
            bestSize = candidateSize;
            matchCount++;
            // First valid match wins (largest-first ordering provides best discrimination)
            break;
        }
    }

    // If ambiguous (shouldn't happen with largest-first), use look-ahead as tiebreaker
    if (matchCount > 1 && bestSize >= 0) {
        for (size_t c = 0; c < kNumKnownSizes; ++c) {
            int sz = kKnownSizes[c];
            if (pos + sz > checkEnd) continue;
            size_t nextPos = pos + sz;
            if (nextPos == checkEnd)
                return sz;
            if (nextPos < checkEnd) {
                uint8_t nextType = data[nextPos] ^ xorByte;
                if (g_typeIDs.count(nextType) > 0)
                    return sz;
            }
        }
    }

    return bestSize;
}

} // anonymous namespace

// ===========================================================================
// Public API
// ===========================================================================

void Reset()
{
    g_hasTypeIDs = false;
    g_allSizesKnown = false;
    g_stringCount = 0;
    g_typeIDs.clear();
    g_typeSizes.clear();
    g_moduleRuntimeBase = 0;
    g_moduleRuntimeSize = 0;
    g_scanBaseAddr = 0;
    LOG(INFO) << "[WARDEN_SCAN] State reset for new module";
}

bool ScanAndExtractTypeIDs()
{
    if (g_hasTypeIDs)
        return true;

    LOG(INFO) << "[WARDEN_SCAN] Scanning process memory for Warden module dispatcher...";

    MEMORY_BASIC_INFORMATION mbi;
    uintptr_t addr = 0;
    int regionsScanned = 0;
    int regionsSkippedSize = 0;
    int regionsSkippedType = 0;

    while (VirtualQuery(reinterpret_cast<LPCVOID>(addr), &mbi, sizeof(mbi)) == sizeof(mbi)) {
        uintptr_t nextAddr = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
        if (nextAddr <= addr)
            break; // overflow protection

        if ((mbi.State & MEM_COMMIT) &&
            (mbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ |
                            PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)))
        {
            // Only scan MEM_PRIVATE regions — Warden module uses VirtualAlloc.
            // MEM_IMAGE (WoW.exe, DLLs) causes false positives from normal x86 code.
            if (mbi.Type == MEM_PRIVATE) {
                if (mbi.RegionSize <= kMaxRegionSize) {
                    regionsScanned++;
                    if (ScanRegionForDispatcher(
                            reinterpret_cast<uintptr_t>(mbi.BaseAddress), mbi.RegionSize))
                        return true;
                } else {
                    regionsSkippedSize++;
                }
            } else {
                regionsSkippedType++;
            }
        }

        addr = nextAddr;
    }

    LOG(INFO) << "[WARDEN_SCAN] No dispatcher found (scanned "
              << regionsScanned << " executable regions, skipped "
              << regionsSkippedSize << " oversized, "
              << regionsSkippedType << " non-private)";
    return false;
}

void LogModuleHeader(const uint8_t* data, size_t size)
{
    if (!data || size < 0x28) {
        LOG(WARNING) << "[WARDEN_SCAN] Module too small for header (" << size << " bytes)";
        return;
    }

    // 40-byte custom Blizzard header (NOT PE)
    uint32_t moduleSize, reserved, relocDataOff, relocCount;
    uint32_t exportTableOff, exportCount, baseIndex;
    uint32_t importTableOff, importLibCount, sectionDescCount;

    std::memcpy(&moduleSize,      data + 0x00, 4);
    std::memcpy(&reserved,        data + 0x04, 4);
    std::memcpy(&relocDataOff,    data + 0x08, 4);
    std::memcpy(&relocCount,      data + 0x0C, 4);
    std::memcpy(&exportTableOff,  data + 0x10, 4);
    std::memcpy(&exportCount,     data + 0x14, 4);
    std::memcpy(&baseIndex,       data + 0x18, 4);
    std::memcpy(&importTableOff,  data + 0x1C, 4);
    std::memcpy(&importLibCount,  data + 0x20, 4);
    std::memcpy(&sectionDescCount, data + 0x24, 4);

    size_t packedDataOff = 0x28 + static_cast<size_t>(sectionDescCount) * 12;

    LOG(INFO) << "[WARDEN_SCAN] Module header (decompressed " << size << " bytes):"
              << " runtimeSize=" << moduleSize
              << " relocCount=" << relocCount
              << " exportOff=0x" << std::hex << exportTableOff
              << " exports=" << std::dec << exportCount
              << " baseIdx=" << baseIndex
              << " importOff=0x" << std::hex << importTableOff
              << " importLibs=" << std::dec << importLibCount
              << " sections=" << sectionDescCount
              << " packedDataAt=0x" << std::hex << packedDataOff;
}

// ---------------------------------------------------------------------------
// Unpack RLE-compressed Warden module binary into a runtime memory image.
// Format: alternating COPY/SKIP entries starting with COPY.
//   COPY: uint16_t LE length + `length` literal bytes
//   SKIP: uint16_t LE length (zero-filled gap)
// Returns empty vector on failure.
// ---------------------------------------------------------------------------
std::vector<uint8_t> UnpackRLE(const uint8_t* data, size_t size)
{
    if (size < 0x28)
        return {};

    uint32_t moduleSize, sectionDescCount;
    std::memcpy(&moduleSize, data + 0x00, 4);
    std::memcpy(&sectionDescCount, data + 0x24, 4);

    if (sectionDescCount == 0 || sectionDescCount >= 32 || moduleSize == 0 || moduleSize > 256 * 1024)
        return {};

    // First section's virtualAddr = destination start
    uint32_t destStart;
    std::memcpy(&destStart, data + 0x28, 4);
    if (destStart >= moduleSize)
        return {};

    size_t srcPos = 0x28 + static_cast<size_t>(sectionDescCount) * 12;
    if (srcPos >= size)
        return {};

    std::vector<uint8_t> image(moduleSize, 0);

    // Copy 40-byte header verbatim
    std::memcpy(image.data(), data, 0x28);

    size_t destPos = destStart;
    bool isSkip = false; // first entry is always COPY

    while (destPos < moduleSize) {
        if (srcPos + 2 > size)
            return {}; // source exhausted prematurely

        uint16_t length;
        std::memcpy(&length, data + srcPos, 2);
        srcPos += 2;

        if (!isSkip) {
            // COPY
            if (srcPos + length > size || destPos + length > moduleSize)
                return {};
            std::memcpy(image.data() + destPos, data + srcPos, length);
            srcPos += length;
        } else {
            // SKIP (image already zeroed)
            if (destPos + length > moduleSize)
                return {};
        }

        destPos += length;
        isSkip = !isSkip;
    }

    LOG(INFO) << "[WARDEN_SCAN] RLE unpack: " << size << " -> " << moduleSize
              << " bytes (consumed " << srcPos << "/" << size << ")";
    return image;
}

bool ScanModuleBinary(const uint8_t* data, size_t size)
{
    if (g_hasTypeIDs)
        return true;

    if (!data || size < kBinaryMinUniqueTypes * 2) {
        LOG(WARNING) << "[WARDEN_SCAN] Module binary too small for scan (" << size << " bytes)";
        return false;
    }

    // Strategy 0 (primary): RLE unpack → scan actual x86 code
    // The decompressed binary is still RLE-packed; scanning it raw produces
    // false positive XOR+MOVZX matches from RLE control words.
    std::vector<uint8_t> unpacked = UnpackRLE(data, size);
    if (!unpacked.empty()) {
        LOG(INFO) << "[WARDEN_SCAN] Scanning RLE-unpacked module ("
                  << unpacked.size() << " bytes) for dispatcher...";
        if (ScanForDispatchChainInBinary(unpacked.data(), unpacked.size(), 0))
            return true;

        // cmp-cluster and sub-chain on unpacked image
        size_t offset = 0;
        if (ScanBufferForDispatcher(unpacked.data(), unpacked.size(),
                                    kBinaryDispatcherWindow, kBinaryMinUniqueTypes, &offset))
            return true;
        if (ScanBufferForSubChain(unpacked.data(), unpacked.size(),
                                   kBinaryDispatcherWindow, kBinaryMinUniqueTypes, &offset))
            return true;

        LOG(INFO) << "[WARDEN_SCAN] No dispatcher in RLE-unpacked image, "
                  << "falling back to raw packed scan...";
    }

    // Fallback: scan raw packed data (legacy path)
    size_t scanStart = 0;
    if (size >= 0x28) {
        uint32_t sdc;
        std::memcpy(&sdc, data + 0x24, 4);
        if (sdc < 32) {
            scanStart = 0x28 + static_cast<size_t>(sdc) * 12;
            if (scanStart >= size)
                scanStart = 0;
        }
    }

    const uint8_t* scanBuf = data + scanStart;
    size_t scanSize = size - scanStart;

    LOG(INFO) << "[WARDEN_SCAN] Scanning raw packed binary ("
              << size << " bytes, offset 0x"
              << std::hex << scanStart << ", " << std::dec << scanSize
              << " bytes) for dispatcher...";

    if (ScanForDispatchChainInBinary(data, size, scanStart))
        return true;

    size_t offset = 0;
    if (ScanBufferForDispatcher(scanBuf, scanSize,
                                kBinaryDispatcherWindow, kBinaryMinUniqueTypes, &offset))
        return true;

    if (ScanBufferForSubChain(scanBuf, scanSize,
                               kBinaryDispatcherWindow, kBinaryMinUniqueTypes, &offset))
        return true;

    LOG(INFO) << "[WARDEN_SCAN] No dispatcher found in module binary";
    return false;
}

// ---------------------------------------------------------------------------
// Scan the in-memory (unpacked, relocated) Warden module for dispatcher.
// Uses g_moduleRuntimeBase/Size set by FindModuleInMemory().
// This is the PRIMARY scan strategy — the in-memory module has actual x86 code
// without RLE packing artifacts that cause false positives in the packed binary.
// ---------------------------------------------------------------------------
bool ScanModuleInMemory()
{
    if (g_hasTypeIDs)
        return true;

    if (g_moduleRuntimeBase == 0 || g_moduleRuntimeSize == 0) {
        LOG(INFO) << "[WARDEN_SCAN] No module runtime address — can't scan in-memory";
        return false;
    }

    LOG(INFO) << "[WARDEN_SCAN] Scanning in-memory module at 0x"
              << std::hex << std::uppercase << std::setfill('0')
              << std::setw(8) << g_moduleRuntimeBase
              << " (" << std::dec << g_moduleRuntimeSize << " bytes)...";

    std::vector<uint8_t> buf(g_moduleRuntimeSize);
    if (!SafeMemcpy(buf.data(), reinterpret_cast<const void*>(g_moduleRuntimeBase),
                    g_moduleRuntimeSize)) {
        LOG(WARNING) << "[WARDEN_SCAN] Failed to read in-memory module at 0x"
                     << std::hex << g_moduleRuntimeBase;
        return false;
    }

    // Set base address so displacement-based offsets (remap tables, jtables)
    // are adjusted from absolute to buffer-relative
    g_scanBaseAddr = g_moduleRuntimeBase;

    // Strategy 1 (primary): XOR-anchored dispatch chain detection
    if (ScanForDispatchChainInBinary(buf.data(), g_moduleRuntimeSize, 0)) {
        g_scanBaseAddr = 0;
        return true;
    }

    // Strategy 2 (fallback): cmp-based dispatcher
    size_t offset = 0;
    if (ScanBufferForDispatcher(buf.data(), g_moduleRuntimeSize,
                                kBinaryDispatcherWindow, kBinaryMinUniqueTypes, &offset)) {
        std::vector<uint8_t> sorted(g_typeIDs.begin(), g_typeIDs.end());
        std::sort(sorted.begin(), sorted.end());

        std::ostringstream oss;
        oss << "[WARDEN_SCAN] Found cmp-based dispatcher in in-memory module at offset 0x"
            << std::hex << std::uppercase << std::setfill('0')
            << std::setw(4) << offset
            << ", extracted " << std::dec << sorted.size() << " check type IDs:";
        for (uint8_t id : sorted)
            oss << " 0x" << std::hex << std::setfill('0') << std::setw(2)
                << static_cast<int>(id);
        LOG(INFO) << oss.str();
        return true;
    }

    // Strategy 3 (fallback): sub chain dispatcher
    offset = 0;
    if (ScanBufferForSubChain(buf.data(), g_moduleRuntimeSize,
                               kBinaryDispatcherWindow, kBinaryMinUniqueTypes, &offset)) {
        std::vector<uint8_t> sorted(g_typeIDs.begin(), g_typeIDs.end());
        std::sort(sorted.begin(), sorted.end());

        std::ostringstream oss;
        oss << "[WARDEN_SCAN] Found sub-chain dispatcher in in-memory module at offset 0x"
            << std::hex << std::uppercase << std::setfill('0')
            << std::setw(4) << offset
            << ", extracted " << std::dec << sorted.size()
            << " check type IDs (cumulative reconstruction):";
        for (uint8_t id : sorted)
            oss << " 0x" << std::hex << std::setfill('0') << std::setw(2)
                << static_cast<int>(id);
        LOG(INFO) << oss.str();
        return true;
    }

    g_scanBaseAddr = 0;
    LOG(INFO) << "[WARDEN_SCAN] No dispatcher found in in-memory module";
    return false;
}

uintptr_t FindModuleInMemory(const uint8_t* moduleBinary, size_t moduleSize)
{
    (void)moduleBinary;
    (void)moduleSize;

    // Documented stable signature: memcpy-like function present in all Warden modules.
    // Not polymorphically varied across modules, unlike encryption functions.
    // Source: SkullSecurity / Warden RE documentation.
    static constexpr uint8_t kStableSig[] = {
        0x56, 0x57, 0xFC, 0x8B, 0x54, 0x24, 0x14,
        0x8B, 0x74, 0x24, 0x10, 0x8B, 0x44, 0x24, 0x0C,
        0x8B, 0xCA, 0x8B, 0xF8, 0xC1, 0xE9, 0x02,
        0x74, 0x02, 0xF3, 0xA5
    };
    constexpr size_t kSigLen = sizeof(kStableSig);

    LOG(INFO) << "[WARDEN_SCAN] Searching MEM_PRIVATE RWX regions for stable module signature ("
              << kSigLen << " bytes)...";

    MEMORY_BASIC_INFORMATION mbi;
    uintptr_t addr = 0;
    int regionsScanned = 0;

    while (VirtualQuery(reinterpret_cast<LPCVOID>(addr), &mbi, sizeof(mbi)) == sizeof(mbi)) {
        uintptr_t nextAddr = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
        if (nextAddr <= addr)
            break;

        if ((mbi.State & MEM_COMMIT) &&
            (mbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ |
                            PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) &&
            (mbi.Type == MEM_PRIVATE) &&
            mbi.RegionSize >= kSigLen &&
            mbi.RegionSize <= kMaxRegionSize)
        {
            regionsScanned++;
            std::vector<uint8_t> buf(mbi.RegionSize);
            if (SafeMemcpy(buf.data(), mbi.BaseAddress, mbi.RegionSize)) {
                for (size_t i = 0; i + kSigLen <= mbi.RegionSize; ++i) {
                    if (std::memcmp(buf.data() + i, kStableSig, kSigLen) == 0) {
                        // Module base = region's AllocationBase (single VirtualAlloc)
                        uintptr_t regionBase = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
                        uintptr_t allocBase  = reinterpret_cast<uintptr_t>(mbi.AllocationBase);
                        uintptr_t sigAddr    = regionBase + i;
                        LOG(INFO) << "[WARDEN_SCAN] Found module in memory!"
                                  << " sig at 0x" << std::hex << std::uppercase
                                  << std::setfill('0') << std::setw(8) << sigAddr
                                  << " region=0x" << std::setw(8) << regionBase
                                  << " allocBase=0x" << std::setw(8) << allocBase
                                  << " regionSize=0x" << std::setw(6) << mbi.RegionSize;
                        g_moduleRuntimeBase = allocBase;
                        g_moduleRuntimeSize = mbi.RegionSize;
                        return allocBase;
                    }
                }
            }
        }

        addr = nextAddr;
    }

    LOG(INFO) << "[WARDEN_SCAN] Module not found in memory (scanned "
              << regionsScanned << " regions)";
    return 0;
}

bool HasTypeIDs()
{
    return g_hasTypeIDs;
}

bool IsValidType(uint8_t id)
{
    return g_typeIDs.count(id) > 0;
}

void SetStringCount(size_t count)
{
    g_stringCount = count;
}

bool AssignTypeSizes(const uint8_t* data, size_t checkStart,
                     size_t checkEnd, uint8_t xorByte)
{
    if (g_allSizesKnown)
        return true;

    if (checkStart >= checkEnd)
        return true; // empty check section is valid

    if (!g_hasTypeIDs) {
        LOG(WARNING) << "[WARDEN_SCAN] No type IDs from module scan — cannot assign sizes";
        return false;
    }

    bool newAssignments = false;
    size_t pos = checkStart;

    while (pos < checkEnd) {
        uint8_t realType = data[pos] ^ xorByte;
        pos++; // consume type byte

        if (!g_typeIDs.count(realType)) {
            LOG(WARNING) << "[WARDEN_SCAN] Unknown type 0x" << std::hex << std::setfill('0')
                         << std::setw(2) << (int)realType
                         << " at offset " << std::dec << (pos - 1)
                         << " — not in module type set";
            return false;
        }

        auto it = g_typeSizes.find(realType);
        if (it != g_typeSizes.end() && it->second >= 0) {
            // Already assigned — skip data bytes
            pos += static_cast<size_t>(it->second);
            continue;
        }

        // Known type with unknown size — structural validation
        int assignedSize = TryAssignSize(data, pos, checkEnd, g_stringCount, xorByte);

        if (assignedSize < 0) {
            LOG(WARNING) << "[WARDEN_SCAN] Failed to assign size for type 0x"
                         << std::hex << std::setfill('0') << std::setw(2)
                         << (int)realType << " at offset " << std::dec << (pos - 1);
            return false;
        }

        g_typeSizes[realType] = assignedSize;
        newAssignments = true;
        LOG(INFO) << "[WARDEN_SCAN] Assigned type 0x" << std::hex << std::setfill('0')
                  << std::setw(2) << (int)realType << " = "
                  << std::dec << assignedSize << " bytes ("
                  << GetTypeName(realType) << ")";

        pos += static_cast<size_t>(assignedSize);
    }

    if (pos != checkEnd) {
        LOG(WARNING) << "[WARDEN_SCAN] Parse overshot check section end (pos="
                     << pos << ", end=" << checkEnd << ")";
        return false;
    }

    if (newAssignments) {
        std::vector<uint8_t> sorted(g_typeIDs.begin(), g_typeIDs.end());
        std::sort(sorted.begin(), sorted.end());
        std::ostringstream oss;
        oss << "[WARDEN_SCAN] Known types (" << std::dec << sorted.size() << "):";
        for (uint8_t id : sorted)
            oss << " 0x" << std::hex << std::setfill('0') << std::setw(2)
                << static_cast<int>(id)
                << "=" << std::dec << g_typeSizes[id] << "B";
        LOG(INFO) << oss.str();
    }

    // Check if all sizes are now known
    g_allSizesKnown = true;
    for (const auto& entry : g_typeSizes) {
        if (entry.second < 0) {
            g_allSizesKnown = false;
            break;
        }
    }

    if (g_allSizesKnown)
        LOG(INFO) << "[WARDEN_SCAN] All check type sizes are now known";

    return true;
}

int GetDataSize(uint8_t id)
{
    auto it = g_typeSizes.find(id);
    if (it != g_typeSizes.end())
        return it->second;
    return -1;
}

bool AllSizesKnown()
{
    return g_allSizesKnown;
}

const char* GetTypeName(uint8_t id)
{
    int size = GetDataSize(id);
    switch (size) {
    case 0:  return "TIMING";
    case 1:  return "MPQ/LUA";
    case 6:  return "MEM_CHECK";
    case 24: return "MODULE";
    case 25: return "DRIVER";
    case 29: return "PAGE";
    case 31: return "PROC";
    default: return "UNKNOWN";
    }
}

uintptr_t GetModuleRuntimeAddress()
{
    return g_moduleRuntimeBase;
}

size_t GetModuleRuntimeSize()
{
    return g_moduleRuntimeSize;
}

} // namespace warden_scan
