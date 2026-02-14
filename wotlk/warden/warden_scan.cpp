#include "warden_scan.h"

#define NOMINMAX
#include <Windows.h>
#include <glog/logging.h>

#include <cstdint>
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

// Candidate data sizes for DFS solver (must match real Warden check sizes only)
constexpr int kCandidateSizes[] = { 0, 1, 2, 25, 27 };
constexpr size_t kNumCandidates = sizeof(kCandidateSizes) / sizeof(kCandidateSizes[0]);

// Maximum unique type IDs to discover in blind mode (prevents combinatorial explosion)
// Real Warden has 9 types; allow slight margin but not too much
constexpr int kMaxUniqueTypes = 10;
// Maximum types that can be assigned size 0 (TIMING).
// Real Warden has exactly 1 TIMING type; limit to 2 to prevent
// the DFS from using size-0 as universal filler for ambiguous parses.
constexpr int kMaxZeroSizeTypes = 2;
// Minimum check section size for blind DFS (reduces ambiguity)
// With structural validation (MEM_CHECK address/readLen checks), even
// shorter packets can be parsed unambiguously.
constexpr size_t kMinBlindCheckLen = 50;

// Maximum consecutive DFS failures before discarding scan-based type IDs
// (detects false positives from memory scan hitting WoW.exe code)
constexpr int kMaxConsecutiveDfsFailures = 3;

// State
bool g_hasTypeIDs = false;
bool g_allSizesKnown = false;
bool g_typeSizesValidated = false; // true after 2nd successful DFS confirms mapping
bool g_typesFromScan = false; // true if types came from memory/binary scan (not DFS)
int  g_consecutiveDfsFailures = 0;
std::unordered_set<uint8_t> g_typeIDs;
std::unordered_map<uint8_t, int> g_typeSizes; // type -> data size (-1 = unknown)

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
            g_typeSizesValidated = true;
            g_typesFromScan = true;
            g_consecutiveDfsFailures = 0;

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
            g_typeSizesValidated = true;
            g_typesFromScan = true;
            g_consecutiveDfsFailures = 0;

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
                            size_t tgt = ComputeJumpTarget(data, dataSize, joff);
                            if (tgt != kNoMatch)
                                queue.push_back({tgt, 0});
                            if (IsJeAt(data, dataSize, afterJcc)) {
                                out.insert(imm);
                                afterJcc += (data[afterJcc] == 0x0F) ? 6 : 2;
                            }
                            pos = afterJcc;
                            handled = true;
                        } else if (IsLessOrEq(jk)) {
                            size_t tgt = ComputeJumpTarget(data, dataSize, joff);
                            if (tgt != kNoMatch)
                                queue.push_back({tgt, 0});
                            pos = afterJcc;
                            handled = true;
                        } else if (IsStrictLess(jk)) {
                            size_t tgt = ComputeJumpTarget(data, dataSize, joff);
                            if (tgt != kNoMatch)
                                queue.push_back({tgt, 0});
                            if (IsJeAt(data, dataSize, afterJcc)) {
                                out.insert(imm);
                                afterJcc += (data[afterJcc] == 0x0F) ? 6 : 2;
                            }
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
                        size_t tgt = ComputeJumpTarget(data, dataSize, joff);
                        if (tgt != kNoMatch) queue.push_back({tgt, 0});
                        if (IsJeAt(data, dataSize, afterJcc)) {
                            out.insert(imm);
                            afterJcc += (data[afterJcc] == 0x0F) ? 6 : 2;
                        }
                        pos = afterJcc;
                        handled = true;
                    } else if (IsLessOrEq(jk)) {
                        size_t tgt = ComputeJumpTarget(data, dataSize, joff);
                        if (tgt != kNoMatch) queue.push_back({tgt, 0});
                        pos = afterJcc;
                        handled = true;
                    } else if (IsStrictLess(jk)) {
                        size_t tgt = ComputeJumpTarget(data, dataSize, joff);
                        if (tgt != kNoMatch) queue.push_back({tgt, 0});
                        if (IsJeAt(data, dataSize, afterJcc)) {
                            out.insert(imm);
                            afterJcc += (data[afterJcc] == 0x0F) ? 6 : 2;
                        }
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
                        size_t tgt = ComputeJumpTarget(data, dataSize, joff);
                        if (tgt != kNoMatch) queue.push_back({tgt, 0});
                        if (IsJeAt(data, dataSize, afterJcc)) {
                            out.insert(imm);
                            afterJcc += (data[afterJcc] == 0x0F) ? 6 : 2;
                        }
                        pos = afterJcc; handled = true;
                    } else if (IsLessOrEq(jk)) {
                        size_t tgt = ComputeJumpTarget(data, dataSize, joff);
                        if (tgt != kNoMatch) queue.push_back({tgt, 0});
                        pos = afterJcc; handled = true;
                    } else if (IsStrictLess(jk)) {
                        size_t tgt = ComputeJumpTarget(data, dataSize, joff);
                        if (tgt != kNoMatch) queue.push_back({tgt, 0});
                        if (IsJeAt(data, dataSize, afterJcc)) {
                            out.insert(imm);
                            afterJcc += (data[afterJcc] == 0x0F) ? 6 : 2;
                        }
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
                            size_t tgt = ComputeJumpTarget(data, dataSize, joff);
                            if (tgt != kNoMatch) queue.push_back({tgt, 0});
                            if (IsJeAt(data, dataSize, afterJcc)) {
                                out.insert(imm);
                                afterJcc += (data[afterJcc] == 0x0F) ? 6 : 2;
                            }
                            pos = afterJcc; handled = true;
                        } else if (IsLessOrEq(jk)) {
                            size_t tgt = ComputeJumpTarget(data, dataSize, joff);
                            if (tgt != kNoMatch) queue.push_back({tgt, 0});
                            pos = afterJcc; handled = true;
                        } else if (IsStrictLess(jk)) {
                            size_t tgt = ComputeJumpTarget(data, dataSize, joff);
                            if (tgt != kNoMatch) queue.push_back({tgt, 0});
                            if (IsJeAt(data, dataSize, afterJcc)) {
                                out.insert(imm);
                                afterJcc += (data[afterJcc] == 0x0F) ? 6 : 2;
                            }
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
        if (disp <= 0x100) continue;
        if (disp < dataSize && disp + 256 > dataSize) continue;
        // Look for jmp [reg*4 + disp] nearby
        for (size_t k = j + 7; k + 6 < dataSize && k < j + 25; ++k) {
            if (data[k] == 0xFF && data[k + 1] == 0x24) {
                uint8_t sib = data[k + 2];
                if ((sib >> 6) == 2 && (sib & 7) == 5) { // scale=4, base=disp32
                    uint32_t jtDisp;
                    std::memcpy(&jtDisp, data + k + 3, 4);
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

        // Count occurrences of each index byte
        uint16_t indexCounts[256] = {};
        for (size_t i = 0; i < tableLen; ++i)
            indexCounts[table[i]]++;

        // Default = most common index
        uint8_t defaultIdx = 0;
        uint16_t maxCount = 0;
        for (int v = 0; v < 256; ++v) {
            if (indexCounts[v] > maxCount) {
                maxCount = indexCounts[v];
                defaultIdx = static_cast<uint8_t>(v);
            }
        }

        // Group type bytes by their index value
        // groups[idx] = list of raw type offsets
        std::unordered_map<uint8_t, std::vector<uint8_t>> groups;
        for (size_t i = 0; i < tableLen; ++i) {
            uint8_t idx = table[i];
            if (idx != defaultIdx)
                groups[idx].push_back(static_cast<uint8_t>(i));
        }

        // Collect singletons + pairs with shift applied
        std::unordered_set<uint8_t> sp;
        for (const auto& entry : groups) {
            if (entry.second.size() <= 2) {
                for (uint8_t raw : entry.second)
                    sp.insert(static_cast<uint8_t>((raw + r.shift) & 0xFF));
            }
        }
        spSets.push_back(sp);

        LOG(INFO) << "[WARDEN_SCAN]   Remap @ 0x" << std::hex << r.remapOff
                  << " (shift=0x" << static_cast<int>(r.shift)
                  << ", max=0x" << static_cast<int>(r.maxType) << "): "
                  << std::dec << sp.size() << " singleton+pair candidates";
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
    std::unordered_set<uint8_t> bestTypes;
    size_t bestXorOff = 0;
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

        // Pre-scan instructions from XOR to movzx for register values
        // (catches patterns like: mov ecx, 0x8E; ...; movzx eax, al; cmp eax, ecx)
        uint8_t preRegVals[8] = {};
        bool preRegValid[8] = {};
        for (size_t p = site.xorOff; p + 4 < site.movzxOff; ) {
            uint8_t b = data[p];
            // mov r32, imm32 (B8+r XX XX XX XX)
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
            size_t len = X86InsnLen(data + p, site.movzxOff - p);
            p += len ? len : 1;
        }

        // Extract dispatch chain types with the proven walker
        std::unordered_set<uint8_t> types;
        ExtractDispatchChainTypes(data, size, site.afterMovzx,
                                   preRegVals, preRegValid, types);

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
        }

        if (types.size() > bestTypes.size()) {
            bestTypes = types;
            bestXorOff = site.xorOff;
        }
    }

    // Step 3: If dispatch chain found, commit results
    if (bestTypes.size() >= kMinChainTypes) {
        g_typeIDs = bestTypes;
        g_typeSizes.clear();
        for (uint8_t id : g_typeIDs)
            g_typeSizes[id] = -1;
        g_hasTypeIDs = true;
        g_allSizesKnown = false;
        g_typeSizesValidated = true;
        g_typesFromScan = true;
        g_consecutiveDfsFailures = 0;

        std::vector<uint8_t> sorted(g_typeIDs.begin(), g_typeIDs.end());
        std::sort(sorted.begin(), sorted.end());
        std::ostringstream oss;
        oss << "[WARDEN_SCAN] Dispatch chain found at XOR @ 0x"
            << std::hex << std::uppercase << std::setfill('0')
            << std::setw(4) << bestXorOff
            << ", " << std::dec << sorted.size() << " check type IDs:";
        for (uint8_t id : sorted)
            oss << " 0x" << std::hex << std::setfill('0') << std::setw(2)
                << static_cast<int>(id);
        LOG(INFO) << oss.str();
        return true;
    }

    // Step 4: No dispatch chain found — try remap table cross-reference
    if (remapInfos.size() >= 2) {
        LOG(INFO) << "[WARDEN_SCAN] No dispatch chain found, trying remap cross-reference ("
                  << remapInfos.size() << " remap tables)...";
        std::unordered_set<uint8_t> remapTypes;
        if (ExtractFromRemapCrossRef(data, size, remapInfos, remapTypes)) {
            g_typeIDs = remapTypes;
            g_typeSizes.clear();
            for (uint8_t id : g_typeIDs)
                g_typeSizes[id] = -1;
            g_hasTypeIDs = true;
            g_allSizesKnown = false;
            g_typeSizesValidated = true;
            g_typesFromScan = true;
            g_consecutiveDfsFailures = 0;

            std::vector<uint8_t> sorted(g_typeIDs.begin(), g_typeIDs.end());
            std::sort(sorted.begin(), sorted.end());
            std::ostringstream oss;
            oss << "[WARDEN_SCAN] Remap cross-reference: " << std::dec << sorted.size()
                << " check type IDs:";
            for (uint8_t id : sorted)
                oss << " 0x" << std::hex << std::setfill('0') << std::setw(2)
                    << static_cast<int>(id);
            LOG(INFO) << oss.str();
            return true;
        }
    }

    if (!bestTypes.empty())
        LOG(INFO) << "[WARDEN_SCAN] Best dispatch chain had only "
                  << bestTypes.size() << " types (need " << kMinChainTypes << ")";
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
    if (ScanForDispatchChainInBinary(buf.data(), regionSize, 0)) {
        LOG(INFO) << "[WARDEN_SCAN] (found in memory region 0x"
                  << std::hex << std::uppercase << std::setfill('0')
                  << std::setw(8) << baseAddr
                  << ", " << std::dec << regionSize << " bytes)";
        return true;
    }

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

        return true;
    }

    return false;
}

// ---------------------------------------------------------------------------
// DFS solver: try to parse check section with candidate sizes.
// Works in two modes:
//   - Informed: g_hasTypeIDs is true, rejects decoded bytes not in g_typeIDs
//   - Blind: g_hasTypeIDs is false, accepts any decoded byte as a potential
//     type (limited to kMaxUniqueTypes to prevent combinatorial explosion)
// ---------------------------------------------------------------------------
bool DFS(const uint8_t* data, size_t pos, size_t end, uint8_t xorByte,
         std::unordered_map<uint8_t, int>& sizeMap)
{
    if (pos == end)
        return true;
    if (pos > end)
        return false;

    uint8_t decoded = data[pos] ^ xorByte;

    // In informed mode, reject unknown type IDs
    if (g_hasTypeIDs && g_typeSizesValidated && g_typeIDs.count(decoded) == 0)
        return false;

    pos++; // consume type byte

    // If we already know this type's size, use it directly
    auto it = sizeMap.find(decoded);
    if (it != sizeMap.end() && it->second >= 0) {
        size_t skip = static_cast<size_t>(it->second);
        if (pos + skip > end)
            return false;
        return DFS(data, pos + skip, end, xorByte, sizeMap);
    }

    // Limit unique types to prevent combinatorial explosion
    int uniqueCount = 0;
    for (const auto& e : sizeMap)
        if (e.second >= 0) uniqueCount++;
    if (uniqueCount >= kMaxUniqueTypes)
        return false;

    // Try each candidate size with backtracking
    for (size_t c = 0; c < kNumCandidates; ++c) {
        int candidateSize = kCandidateSizes[c];
        if (pos + candidateSize > end)
            continue;

        // Limit size-0 types to prevent ambiguous parses where the DFS
        // assigns many types as TIMING (size 0) to pad the parse
        if (candidateSize == 0) {
            int zeroCount = 0;
            for (const auto& e : sizeMap)
                if (e.second == 0) zeroCount++;
            if (zeroCount >= kMaxZeroSizeTypes)
                continue;
        }

        // Structural validation for MEM_CHECK (size 27):
        // Data bytes are plaintext (not XORed). The layout is:
        //   [4B address] [20B SHA1] [1B] [1B] [1B readLen]
        // Reject if address is outside user-mode range or readLen is too large.
        if (candidateSize == 27) {
            uint32_t addr;
            memcpy(&addr, data + pos, 4);
            uint8_t readLen = data[pos + 26];
            if (addr < 0x10000 || addr > 0x7FFFFFFF)
                continue;
            if (readLen == 0 || readLen > 40)
                continue;
        }

        sizeMap[decoded] = candidateSize;
        if (DFS(data, pos + candidateSize, end, xorByte, sizeMap))
            return true;
        sizeMap[decoded] = -1; // backtrack
    }

    return false;
}

} // anonymous namespace

// ===========================================================================
// Public API
// ===========================================================================

void Reset()
{
    g_hasTypeIDs = false;
    g_allSizesKnown = false;
    g_typeSizesValidated = false;
    g_typesFromScan = false;
    g_consecutiveDfsFailures = 0;
    g_typeIDs.clear();
    g_typeSizes.clear();
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

bool ScanModuleBinary(const uint8_t* data, size_t size)
{
    if (g_hasTypeIDs)
        return true;

    if (!data || size < kBinaryMinUniqueTypes * 2) {
        LOG(WARNING) << "[WARDEN_SCAN] Module binary too small for scan (" << size << " bytes)";
        return false;
    }

    // Parse header to find where packed section data starts
    size_t scanStart = 0;
    if (size >= 0x28) {
        uint32_t sectionDescCount;
        std::memcpy(&sectionDescCount, data + 0x24, 4);
        // Sanity check — section count should be small
        if (sectionDescCount < 32) {
            scanStart = 0x28 + static_cast<size_t>(sectionDescCount) * 12;
            if (scanStart >= size)
                scanStart = 0; // fallback to full scan
        }
    }

    const uint8_t* scanBuf = data + scanStart;
    size_t scanSize = size - scanStart;

    LOG(INFO) << "[WARDEN_SCAN] Scanning decompressed module binary ("
              << size << " bytes, scanning from offset 0x"
              << std::hex << scanStart << ", " << std::dec << scanSize
              << " bytes) for dispatcher...";

    // Strategy 1 (primary): XOR-anchored dispatch chain detection
    // Proven across 10 Warden modules via Python/capstone analysis.
    if (ScanForDispatchChainInBinary(data, size, scanStart))
        return true;

    // Strategy 2 (fallback): cmp-based dispatcher (cmp al/eax/r32, imm)
    size_t offset = 0;
    if (ScanBufferForDispatcher(scanBuf, scanSize,
                                kBinaryDispatcherWindow, kBinaryMinUniqueTypes, &offset)) {
        std::vector<uint8_t> sorted(g_typeIDs.begin(), g_typeIDs.end());
        std::sort(sorted.begin(), sorted.end());

        std::ostringstream oss;
        oss << "[WARDEN_SCAN] Found cmp-based dispatcher in module binary at offset 0x"
            << std::hex << std::uppercase << std::setfill('0')
            << std::setw(4) << (scanStart + offset)
            << ", extracted " << std::dec << sorted.size() << " check type IDs:";
        for (uint8_t id : sorted)
            oss << " 0x" << std::hex << std::setfill('0') << std::setw(2)
                << static_cast<int>(id);
        LOG(INFO) << oss.str();

        return true;
    }

    // Strategy 3 (fallback): sub chain dispatcher (sub al/eax, delta; je/jne)
    LOG(INFO) << "[WARDEN_SCAN] No cmp-based dispatcher found, trying sub chain detection...";
    offset = 0;
    if (ScanBufferForSubChain(scanBuf, scanSize,
                               kBinaryDispatcherWindow, kBinaryMinUniqueTypes, &offset)) {
        std::vector<uint8_t> sorted(g_typeIDs.begin(), g_typeIDs.end());
        std::sort(sorted.begin(), sorted.end());

        std::ostringstream oss;
        oss << "[WARDEN_SCAN] Found sub-chain dispatcher in module binary at offset 0x"
            << std::hex << std::uppercase << std::setfill('0')
            << std::setw(4) << (scanStart + offset)
            << ", extracted " << std::dec << sorted.size()
            << " check type IDs (cumulative reconstruction):";
        for (uint8_t id : sorted)
            oss << " 0x" << std::hex << std::setfill('0') << std::setw(2)
                << static_cast<int>(id);
        LOG(INFO) << oss.str();

        return true;
    }

    LOG(INFO) << "[WARDEN_SCAN] No dispatcher found in module binary"
              << " (tried XOR-anchored chain, cmp patterns, and sub chains)";
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

bool LearnSizesFromPacket(const uint8_t* data, size_t checkStart,
                          size_t checkEnd, uint8_t xorByte)
{
    if (g_allSizesKnown && g_typeSizesValidated)
        return true;

    if (checkStart >= checkEnd)
        return false;

    size_t checkLen = checkEnd - checkStart;
    bool blind = !g_hasTypeIDs;

    // In blind mode, require sufficient data to reduce parse ambiguity
    if (blind && checkLen < kMinBlindCheckLen)
        return false;

    // Work on a copy so we don't corrupt state on DFS failure
    std::unordered_map<uint8_t, int> workingMap = g_typeSizes;

    if (!DFS(data, checkStart, checkEnd, xorByte, workingMap)) {
        if (g_hasTypeIDs && !g_typeSizesValidated) {
            // Tentative mapping failed — discard and retry blind
            LOG(WARNING) << "[WARDEN_SCAN] Tentative mapping failed validation, resetting";
            g_typeIDs.clear();
            g_typeSizes.clear();
            g_hasTypeIDs = false;
            if (checkLen < kMinBlindCheckLen)
                return false;
            workingMap.clear();
            if (!DFS(data, checkStart, checkEnd, xorByte, workingMap))
                return false;
            blind = true; // fall through to discovery logic
        } else if (g_hasTypeIDs) {
            g_consecutiveDfsFailures++;
            if (g_typesFromScan && g_consecutiveDfsFailures >= kMaxConsecutiveDfsFailures) {
                // Scan-based types are likely a false positive (e.g. from WoW.exe code)
                LOG(WARNING) << "[WARDEN_SCAN] " << g_consecutiveDfsFailures
                             << " consecutive DFS failures with scan-based types — "
                             << "discarding as likely false positive";
                g_typeIDs.clear();
                g_typeSizes.clear();
                g_hasTypeIDs = false;
                g_allSizesKnown = false;
                g_typeSizesValidated = false;
                g_typesFromScan = false;
                g_consecutiveDfsFailures = 0;
                // Retry blind if packet is large enough
                if (checkLen >= kMinBlindCheckLen) {
                    workingMap.clear();
                    if (!DFS(data, checkStart, checkEnd, xorByte, workingMap))
                        return false;
                    blind = true;
                } else {
                    return false;
                }
            } else {
                LOG(WARNING) << "[WARDEN_SCAN] DFS solver failed to parse check section ("
                             << std::dec << checkLen << " bytes, failure "
                             << g_consecutiveDfsFailures << "/"
                             << kMaxConsecutiveDfsFailures << ")";
                return false;
            }
        } else {
            return false;
        }
    }

    // DFS succeeded — reset failure counter and commit discovered types/sizes
    g_consecutiveDfsFailures = 0;

    // DFS succeeded — commit discovered types and sizes
    bool newTypes = false;
    for (const auto& entry : workingMap) {
        if (entry.second < 0)
            continue;
        if (g_typeIDs.count(entry.first) == 0) {
            g_typeIDs.insert(entry.first);
            g_typeSizes[entry.first] = entry.second;
            newTypes = true;
        } else if (g_typeSizes[entry.first] < 0) {
            g_typeSizes[entry.first] = entry.second;
            newTypes = true;
        }
    }

    if (!g_hasTypeIDs && !g_typeIDs.empty()) {
        g_hasTypeIDs = true;
        // First successful DFS = tentative; mark validated on second success
    } else if (g_hasTypeIDs && !g_typeSizesValidated) {
        g_typeSizesValidated = true;
        LOG(INFO) << "[WARDEN_SCAN] Type mapping validated by second packet";
    }

    if (newTypes) {
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

    return g_allSizesKnown;
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
    case 1:  return "MODULE/DRIVER";
    case 2:  return "LUA";
    case 25: return "PAGE/PROC/MPQ";
    case 27: return "MEM_CHECK";
    default: return "UNKNOWN";
    }
}

} // namespace warden_scan
