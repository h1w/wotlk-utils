#include "warden_rc4_hook.h"
#include "warden_types.h"

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
// Internal RC4 hook: hooks the RC4 PRGA function INSIDE the Warden module
// blob to capture CMSG plaintext directly before encryption.
//
// The Warden module's RC4 context uses [S[256]][i][j] layout, so i/j fields
// are at offsets +0x100 and +0x101 from the context base pointer.
// The scanner finds the RC4 function by looking for MOVZX/MOV instructions
// with these distinctive displacements.
// ===========================================================================

namespace {

static constexpr size_t kMaxCaptureSize = 4096;

// ---------------------------------------------------------------------------
// Hook state
// ---------------------------------------------------------------------------
static void* g_originalRC4 = nullptr;
static bool  g_hookActive  = false;
static uintptr_t g_hookedAddr = 0;

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
// Calling convention detection
// ---------------------------------------------------------------------------
static int  g_callCount       = 0;
static bool g_conventionKnown = false;
// 0 = unknown
// 1 = __thiscall: ECX=ctx, stk1=data, stk2=len
// 2 = __cdecl:    stk1=ctx, stk2=data, stk3=len
// 3 = variant:    ECX=ctx, stk1=len, stk2=data
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

// ===========================================================================
// Pattern scanner: find RC4 PRGA function in runtime module memory.
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

// Check if a ModRM byte has mod=10 (32-bit displacement) and extract disp position.
// Returns displacement offset from instruction start, or 0 if not mod=10.
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
    // Opcodes to scan: each has ModRM at different offset from opcode start
    // {opcode_byte, modrm_offset, is_two_byte_opcode}
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

static uintptr_t ScanRuntimeForRC4(uintptr_t base, size_t size)
{
    if (size < 512) {
        LOG(INFO) << "[RC4_HOOK] Module too small (" << size << " bytes)";
        return 0;
    }

    // Read entire module memory
    std::vector<uint8_t> buf(size);
    if (!SafeReadBytes(reinterpret_cast<const void*>(base), buf.data(), size)) {
        LOG(WARNING) << "[RC4_HOOK] Failed to read module memory at 0x"
                     << std::hex << base;
        return 0;
    }

    // Scan for instructions referencing 0x100/0x101
    std::vector<PatternMatch> matches;
    ScanForDispReferences(buf.data(), size, matches);

    if (matches.empty()) {
        LOG(INFO) << "[RC4_HOOK] No instructions referencing 0x100/0x101 found in module";
        return 0;
    }

    LOG(INFO) << "[RC4_HOOK] Found " << std::dec << matches.size()
              << " instructions referencing 0x100/0x101 in module memory";

    // Sort by offset
    std::sort(matches.begin(), matches.end(),
              [](const PatternMatch& a, const PatternMatch& b) {
                  return a.offset < b.offset;
              });

    // Find best cluster (most matches within 120-byte window)
    size_t bestI = 0, bestJ = 0, bestCount = 0;
    for (size_t i = 0; i < matches.size(); ++i) {
        size_t j = i;
        while (j < matches.size() &&
               matches[j].offset - matches[i].offset <= 120)
            ++j;
        if (j - i > bestCount) {
            bestCount = j - i;
            bestI = i;
            bestJ = j;
        }
    }

    if (bestCount < 2) {
        LOG(INFO) << "[RC4_HOOK] No cluster with >= 2 references to 0x100/0x101";
        return 0;
    }

    // Check cluster has both 0x100 and 0x101 references
    bool has100 = false, has101 = false;
    for (size_t k = bestI; k < bestJ; ++k) {
        if (matches[k].disp == 0x100) has100 = true;
        if (matches[k].disp == 0x101) has101 = true;
    }

    size_t clusterStart = matches[bestI].offset;
    size_t clusterEnd   = matches[bestJ - 1].offset;

    LOG(INFO) << "[RC4_HOOK] Best cluster: " << std::dec << bestCount
              << " matches at module offsets 0x" << std::hex << clusterStart
              << "-0x" << clusterEnd
              << (has100 ? " [has i]" : " [NO i]")
              << (has101 ? " [has j]" : " [NO j]");

    if (!has100 || !has101) {
        LOG(WARNING) << "[RC4_HOOK] Cluster missing reference to i(0x100) or j(0x101)"
                     << " — may not be RC4 PRGA";
    }

    // Walk backward from cluster start to find function prologue
    size_t funcOffset = clusterStart;
    for (size_t back = 1; back <= 256 && back <= clusterStart; ++back) {
        size_t pos = clusterStart - back;

        // push ebp; mov ebp, esp (55 8B EC)
        if (pos + 2 < size &&
            buf[pos] == 0x55 && buf[pos + 1] == 0x8B && buf[pos + 2] == 0xEC)
        {
            // Verify preceded by function boundary
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

    uintptr_t absoluteAddr = base + funcOffset;
    LOG(INFO) << "[RC4_HOOK] RC4 PRGA function at runtime address 0x"
              << std::hex << std::setfill('0') << std::setw(8) << absoluteAddr
              << " (module+0x" << funcOffset << ")"
              << " cluster at module+0x" << clusterStart << "-0x" << clusterEnd;

    // Log first bytes at function start for diagnostics
    size_t dumpLen = (size - funcOffset < 32) ? (size - funcOffset) : 32;
    LOG(INFO) << "[RC4_HOOK] Function prologue bytes: ["
              << BytesToHex(buf.data() + funcOffset, dumpLen) << "]";

    return absoluteAddr;
}

// ===========================================================================
// Naked hook and detour handler
// ===========================================================================

// Stack layout after pushad+pushfd (36 bytes):
//   s[0]=EFLAGS s[1]=EDI s[2]=ESI s[3]=EBP s[4]=ESP_orig
//   s[5]=EBX s[6]=EDX s[7]=ECX s[8]=EAX
//   s[9]=retaddr s[10]=stk1 s[11]=stk2 s[12]=stk3

static void __cdecl RC4DetourHandler(uintptr_t savedEsp)
{
    uint32_t* s = reinterpret_cast<uint32_t*>(savedEsp);

    uint32_t ecx  = s[7];
    uint32_t edx  = s[6];
    uint32_t stk1 = s[10];
    uint32_t stk2 = s[11];
    uint32_t stk3 = s[12];

    int callNum = ++g_callCount;

    // Diagnostic logging for first few calls
    if (callNum <= 5) {
        LOG(INFO) << "[RC4_HOOK] call#" << std::dec << callNum
                  << " ECX=0x" << std::hex << std::setfill('0') << std::setw(8) << ecx
                  << " EDX=0x" << std::setw(8) << edx
                  << " stk1=0x" << std::setw(8) << stk1
                  << " stk2=0x" << std::setw(8) << stk2
                  << " stk3=0x" << std::setw(8) << stk3;
    }

    // --- Calling convention auto-detection ---
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
        // Try __cdecl: stk1=ctx, stk2=data, stk3=len
        else if (LooksLikeRC4Context(stk1) &&
                 IsValidPointer(stk2) && IsReasonableLength(stk3))
        {
            g_convention = 2;
            g_conventionKnown = true;
            LOG(INFO) << "[RC4_HOOK] Convention detected: __cdecl"
                      << " (stk1=ctx, stk2=data, stk3=len)";
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
        // Try: EDX=ctx, stk1=data, stk2=len (unlikely but check)
        else if (LooksLikeRC4Context(edx) &&
                 IsValidPointer(stk1) && IsReasonableLength(stk2))
        {
            g_convention = 4;
            g_conventionKnown = true;
            LOG(INFO) << "[RC4_HOOK] Convention detected: EDX-variant"
                      << " (EDX=ctx, stk1=data, stk2=len)";
        }
    }

    // --- Extract parameters based on known convention ---
    uint32_t dataPtr = 0, dataLen = 0;

    switch (g_convention) {
    case 1: dataPtr = stk1; dataLen = stk2; break;
    case 2: dataPtr = stk2; dataLen = stk3; break;
    case 3: dataPtr = stk2; dataLen = stk1; break;
    case 4: dataPtr = stk1; dataLen = stk2; break;
    default:
        // Convention unknown — try heuristic for immediate capture
        if (IsValidPointer(stk1) && IsReasonableLength(stk2)) {
            dataPtr = stk1; dataLen = stk2;
        } else if (IsValidPointer(stk2) && IsReasonableLength(stk3)) {
            dataPtr = stk2; dataLen = stk3;
        } else if (IsReasonableLength(stk1) && IsValidPointer(stk2)) {
            dataPtr = stk2; dataLen = stk1;
        }
        break;
    }

    if (!IsValidPointer(dataPtr) || !IsReasonableLength(dataLen))
        return;

    // Read data buffer BEFORE the original RC4 function modifies it.
    // For encrypt calls: this is the plaintext CMSG.
    // For decrypt calls: this is the encrypted SMSG (will fail validation).
    uint8_t localBuf[kMaxCaptureSize];
    if (!SafeReadBytes(reinterpret_cast<const void*>(dataPtr), localBuf, dataLen))
        return;

    // Validate as CMSG — only encrypt calls produce valid CMSG structure
    if (!ValidateDecryptedCmsg(localBuf, dataLen))
        return;

    // Valid CMSG plaintext captured
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

__declspec(naked) static void HookedRC4Naked()
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

        jmp dword ptr [g_originalRC4]
    }
}

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

    if (g_hookActive) {
        LOG(INFO) << "[RC4_HOOK] Already active at 0x" << std::hex << g_hookedAddr
                  << ", removing before reinstall";
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

    uintptr_t funcAddr = ScanRuntimeForRC4(moduleBase, moduleSize);
    if (funcAddr == 0) {
        LOG(WARNING) << "[RC4_HOOK] RC4 PRGA function not found in module at 0x"
                     << std::hex << moduleBase << " (size=0x" << moduleSize << ")";
        return false;
    }

    MH_STATUS status = MH_CreateHook(
        reinterpret_cast<LPVOID>(funcAddr),
        reinterpret_cast<LPVOID>(&HookedRC4Naked),
        &g_originalRC4);

    if (status != MH_OK) {
        LOG(ERROR) << "[RC4_HOOK] MH_CreateHook(0x" << std::hex << funcAddr
                   << ") failed: " << MH_StatusToString(status);
        return false;
    }

    status = MH_EnableHook(reinterpret_cast<LPVOID>(funcAddr));
    if (status != MH_OK) {
        LOG(ERROR) << "[RC4_HOOK] MH_EnableHook(0x" << std::hex << funcAddr
                   << ") failed: " << MH_StatusToString(status);
        MH_RemoveHook(reinterpret_cast<LPVOID>(funcAddr));
        return false;
    }

    g_hookedAddr = funcAddr;
    g_hookActive = true;

    LOG(INFO) << "[RC4_HOOK] Hook installed at 0x"
              << std::hex << std::setfill('0') << std::setw(8) << funcAddr
              << " (module base=0x" << std::setw(8) << moduleBase
              << " size=0x" << moduleSize << ")";
    return true;
}

void Remove()
{
    if (!g_hookActive)
        return;

    MH_STATUS status = MH_DisableHook(reinterpret_cast<LPVOID>(g_hookedAddr));
    if (status != MH_OK) {
        LOG(WARNING) << "[RC4_HOOK] MH_DisableHook(0x" << std::hex << g_hookedAddr
                     << ") failed: " << MH_StatusToString(status);
    }

    status = MH_RemoveHook(reinterpret_cast<LPVOID>(g_hookedAddr));
    if (status != MH_OK) {
        LOG(WARNING) << "[RC4_HOOK] MH_RemoveHook(0x" << std::hex << g_hookedAddr
                     << ") failed: " << MH_StatusToString(status);
    }

    LOG(INFO) << "[RC4_HOOK] Hook removed from 0x"
              << std::hex << std::setfill('0') << std::setw(8) << g_hookedAddr
              << " (total calls: " << std::dec << g_callCount << ")";

    g_hookActive    = false;
    g_hookedAddr    = 0;
    g_originalRC4   = nullptr;
}

bool IsActive()
{
    return g_hookActive;
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
