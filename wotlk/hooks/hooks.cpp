#include "hooks.h"

#define NOMINMAX
#include <Windows.h>
#include <MinHook.h>
#include <glog/logging.h>

#include "../warden/warden_types.h"
#include "../warden/module_dump.h"
#include "../warden/warden_scan.h"
#include "../warden/warden_rc4.h"
#include "../warden/warden_rc4_hook.h"

#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>
#include <algorithm>
#include <sstream>
#include <iomanip>

// ===========================================================================
// FrameScript::Execute — исполнение Lua-кода игровым движком
// Адрес:      0x00819210  (WoW 3.3.5a build 12340)
// Конвенция:  __cdecl
// ===========================================================================

static constexpr uintptr_t kFrameScriptExecute = 0x00819210;
static constexpr size_t kMaxCodeLogLen = 300;

using FrameScript_Execute_t = void(__cdecl*)(const char* code,
                                              const char* filename,
                                              int         unused);

static FrameScript_Execute_t g_originalFrameScriptExecute = nullptr;

static bool IsStandardScript(const char* filename)
{
    if (!filename || !filename[0])
        return false;

    std::string_view src(filename);

    if (src.find("Interface\\") != std::string_view::npos)
        return true;
    if (src.find("compat.lua") != std::string_view::npos)
        return true;
    if (src[0] == '@')
        return true;

    return false;
}

static void __cdecl HookedFrameScriptExecute(const char* code,
                                              const char* filename,
                                              int         unused)
{
    if (code && !IsStandardScript(filename)) {
        const char* src = (filename && filename[0]) ? filename : "<no source>";

        std::string_view codeView(code);
        if (codeView.size() <= kMaxCodeLogLen) {
            LOG(INFO) << "[LUA] source=\"" << src << "\" code=\"" << codeView << "\"";
        } else {
            LOG(INFO) << "[LUA] source=\"" << src << "\" code=\""
                      << codeView.substr(0, kMaxCodeLogLen) << "...\" ("
                      << codeView.size() << " bytes)";
        }
    }

    g_originalFrameScriptExecute(code, filename, unused);
}

// ===========================================================================
// SMSG_WARDEN_DATA — packet handler для входящих Warden-пакетов
// Адрес:      0x007DA850  (WoW 3.3.5a build 12340)
//
// Calling convention (определено эмпирически):
//   __thiscall-like с 4 стековыми аргументами:
//     ECX        = internal buffer pointer
//     stk1 (0)   = unused/flags
//     stk2       = opcode (0x2E6 = SMSG_WARDEN_DATA)
//     stk3       = duplicate of ECX
//     stk4       = CDataStore* (пакет, readPos=2 после чтения опкода)
//
// ВАЖНО: Данные в CDataStore зашифрованы Warden RC4.
// Расшифровка происходит ВНУТРИ оригинального обработчика.
//
// CDataStore layout (build 12340):
//   +0x00  vtable
//   +0x04  uint8_t*  m_buffer
//   +0x08  uint32_t  m_base
//   +0x0C  uint32_t  m_alloc
//   +0x10  uint32_t  m_size
//   +0x14  uint32_t  m_read
// ===========================================================================

static constexpr uintptr_t kWardenHandler = 0x007DA850;
static constexpr size_t kMaxDecryptedDump = 256;
static constexpr size_t kMaxWardenPayload = 4096;

// Trampoline хранится как void* — naked хук прыгнет напрямую
static void* g_originalWardenHandler = nullptr;

// Return-address hijack state (main-thread only, no concurrency)
static void*    g_savedRetAddr    = nullptr;
static void*    g_savedCDataStore = nullptr;
static uint32_t g_savedReadPos    = 0;

// Address of WardenPostHandlerNaked (set in Initialize, used by PreHandler
// to hijack return address). Can't forward-declare naked functions in MSVC.
static void* g_wardenPostHandlerAddr = nullptr;

// Safety flag: true while hooks are disabled during Warden handler execution
static bool g_hooksDisabled = false;

// ===========================================================================
// SendPacket — перехват исходящих пакетов
// Адрес:      0x00632B50  (WoW 3.3.5a build 12340)
//
// CDataStore содержит [4-byte opcode][payload].
// Warden RC4 encryption уже применена к payload на этом уровне.
// ===========================================================================

static constexpr uintptr_t kSendPacket = 0x00632B50;
static constexpr uint32_t kCmsgWardenDataOpcode = 0x2E7;

static void* g_originalSendPacket = nullptr;
static volatile LONG g_cmsgWardenCount = 0;
static DWORD g_lastSmsgWardenTick = 0;

// Diagnostic: log ALL outgoing packet opcodes
static volatile LONG g_sendPacketDiagCount = 0;

// ===========================================================================
// ARC4::Process — SESSION HEADER cipher (NOT Warden payload cipher)
// Адрес:      0x00774EA0  (WoW 3.3.5a build 12340)
// Конвенция:  __thiscall — ECX=ARC4*, stk1=data(uint8_t*), stk2=len(uint32_t)
//
// This encrypts/decrypts packet headers: CMSG=[2B size][4B opcode]=6 bytes,
// SMSG=4 bytes. Kept for diagnostics only.
// ===========================================================================

static constexpr uintptr_t kARC4Process = 0x00774EA0;
static void* g_originalARC4Process = nullptr;

// Flag: true while inside Warden handler (between Pre and Post)
static bool g_insideWardenHandler = false;
static int  g_wardenArc4CallNum   = 0;

// Timestamp when g_insideWardenHandler was set (for stale-flag detection)
static DWORD g_wardenHandlerStartTick = 0;

// Форматирование байтов в hex-строку
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

// SEH-безопасное чтение полей CDataStore
static bool __cdecl SafeReadCDataStore(void* ptr,
                                       uintptr_t* outBuffer,
                                       uint32_t* outSize,
                                       uint32_t* outReadPos)
{
    __try {
        *outBuffer  = *(uintptr_t*)((uintptr_t)ptr + 0x04);
        *outSize    = *(uint32_t*) ((uintptr_t)ptr + 0x10);
        *outReadPos = *(uint32_t*) ((uintptr_t)ptr + 0x14);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// SEH-безопасное чтение N байт
static bool __cdecl SafeReadBytes(const uint8_t* src, uint8_t* dst, size_t count)
{
    __try {
        for (size_t i = 0; i < count; ++i)
            dst[i] = src[i];
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Packet counter
static uint32_t g_wardenPacketCount = 0;

// ---------------------------------------------------------------------------
// Helper predicates
// ---------------------------------------------------------------------------

static bool IsKnownWardenOpcode(uint8_t op)
{
    switch (op) {
    case WARDEN_SMSG_MODULE_USE:
    case WARDEN_SMSG_MODULE_CACHE:
    case WARDEN_SMSG_CHEAT_CHECKS_REQUEST:
    case WARDEN_SMSG_MODULE_INITIALIZE:
    case WARDEN_SMSG_HASH_REQUEST:
        return true;
    default:
        return false;
    }
}

// ---------------------------------------------------------------------------
// Pre-handler: save CDataStore context + hijack return address
// Called from naked hook BEFORE original handler runs.
//
// Stack layout after pushad+pushfd (36 bytes):
//   s[0]=EFLAGS s[1]=EDI s[2]=ESI s[3]=EBP s[4]=ESP_orig
//   s[5]=EBX s[6]=EDX s[7]=ECX s[8]=EAX
//   s[9]=retaddr s[10]=stk1(0) s[11]=opcode s[12]=bufptr s[13]=CDataStore*
// ---------------------------------------------------------------------------

static void __cdecl WardenPreHandler(uintptr_t savedEsp)
{
    // Mark that we're inside the Warden handler (for ARC4 hook)
    g_insideWardenHandler = true;
    g_wardenArc4CallNum = 0;
    g_wardenHandlerStartTick = GetTickCount();

    // Clone all RC4 S-box candidates BEFORE the handler runs (fallback only)
    // Skip if internal RC4 hook is active (primary method doesn't need cloning)
    if (!warden_rc4_hook::IsActive()) {
        if (warden_rc4::HasEncryptState() || warden_rc4::HasCandidates())
            warden_rc4::CloneAllStates();
    }

    // Restore original bytes at all hook targets so Warden sees clean memory
    if (!g_hooksDisabled) {
        g_hooksDisabled = true;
        MH_DisableHook(reinterpret_cast<LPVOID>(kFrameScriptExecute));
        MH_DisableHook(reinterpret_cast<LPVOID>(kWardenHandler));
    }

    uint32_t* s = (uint32_t*)savedEsp;

    g_savedCDataStore = (void*)s[13];
    g_savedReadPos = 0;

    if (g_savedCDataStore) {
        uintptr_t buf;
        uint32_t sz, rp;
        if (SafeReadCDataStore(g_savedCDataStore, &buf, &sz, &rp))
            g_savedReadPos = rp;
    }

    // Hijack: replace caller's return address with our post-handler
    g_savedRetAddr = (void*)s[9];
    s[9] = (uint32_t)g_wardenPostHandlerAddr;
}

// ---------------------------------------------------------------------------
// CHEAT_CHECKS_REQUEST (0x02) parser
//
// Packet layout: [0x02] [string_section] [check_section] [xorByte]
//
// String section: series of [1-byte len][N-byte string] — for LUA, MPQ,
//   DRIVER, PROC checks (PROC uses two strings).
// Check section: series of [type^xor] [type-specific data].
// Last byte = xorByte used to decode check type bytes.
// ---------------------------------------------------------------------------

// Returns true if all bytes in [src, src+count) are printable ASCII (0x20-0x7E)
static bool __cdecl IsPrintableAscii(const uint8_t* src, size_t count)
{
    __try {
        for (size_t i = 0; i < count; ++i)
            if (src[i] < 0x20 || src[i] > 0x7E)
                return false;
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// ---------------------------------------------------------------------------
// Module-agnostic scan: search raw check section bytes for 4-byte LE patterns
// of our hook addresses.  Works regardless of which Warden module is loaded,
// since only TYPE bytes are XOR'd — address data is plaintext.
// ---------------------------------------------------------------------------

static void ScanForHookAddresses(const uint8_t* data, size_t start, size_t end)
{
    static constexpr struct { uintptr_t addr; const char* name; } kTargets[] = {
        { kFrameScriptExecute, "FrameScript_Execute" },
        { kWardenHandler,      "WardenHandler" },
        { kSendPacket,         "SendPacket" },
        { kARC4Process,        "ARC4_Process" },
    };

    for (const auto& t : kTargets) {
        uint8_t pat[4];
        std::memcpy(pat, &t.addr, 4);

        for (size_t i = start; i + 4 <= end; ++i) {
            if (std::memcmp(data + i, pat, 4) == 0) {
                LOG(WARNING) << "[WARDEN]   *** ADDRESS SCAN: found " << t.name
                             << " (0x" << std::hex << std::setfill('0')
                             << std::setw(8) << t.addr
                             << ") at check offset +" << std::dec << (i - start)
                             << " — probable MEM_CHECK target ***";
            }
        }
    }
}

// Classify a Warden check string by its content
static const char* ClassifyWardenString(const std::string& s)
{
    // MPQ file path (map tiles, models, textures)
    if (s.find('\\') != std::string::npos) {
        if (s.find(".adt") != std::string::npos ||
            s.find(".wmo") != std::string::npos ||
            s.find(".m2")  != std::string::npos ||
            s.find(".blp") != std::string::npos ||
            s.find(".wdl") != std::string::npos ||
            s.find(".wdt") != std::string::npos)
            return "MPQ";
    }
    // Lua variable/expression check
    if (s.find('=') != std::string::npos || s.find('(') != std::string::npos)
        return "LUA_EVAL";
    // Known VM/driver names
    if (s == "vmmemctl" || s == "VBoxMiniRdrDN" || s == "vmci" ||
        s == "vboxguest" || s == "vmhgfs" || s == "prl_fs" ||
        s == "VBoxSF" || s == "VBoxGuest")
        return "DRIVER";
    return "STRING";
}

static void ParseCheatChecksRequest(const uint8_t* data, size_t len)
{
    if (len < 3) return;

    // xorByte is the last byte of the packet (also appears as terminator)
    uint8_t xorByte = data[len - 1];
    LOG(INFO) << "[WARDEN]   xorByte=0x"
              << std::hex << std::setfill('0') << std::setw(2) << (int)xorByte;

    // --- String section ---
    // Strings are [1-byte len][N-byte printable ASCII].  We use a printable
    // heuristic to tell strings apart from the check section, because check
    // type values are module-specific.
    std::vector<std::string> strings;
    size_t pos = 1; // skip opcode byte

    while (pos < len - 1) {
        uint8_t strLen = data[pos];
        if (strLen == 0 || pos + 1 + strLen > len - 1)
            break;
        if (!IsPrintableAscii(data + pos + 1, strLen))
            break;
        strings.emplace_back(reinterpret_cast<const char*>(data + pos + 1), strLen);
        pos += 1 + strLen;
    }

    // Skip 0x00 string section terminator
    if (pos < len - 1 && data[pos] == 0x00)
        pos++;

    for (size_t i = 0; i < strings.size(); ++i)
        LOG(INFO) << "[WARDEN]   string[" << std::dec << i
                  << "] (" << ClassifyWardenString(strings[i])
                  << ") \"" << strings[i] << "\"";

    // --- Check section ---
    // Each entry: [type^xorByte (1 byte)] [type-specific data]
    // Check type IDs are module-specific (determined by RE of module 7C4ABC97).
    size_t checkStart = pos;
    size_t checkEnd   = len - 1; // exclude terminal xorByte

    if (checkStart >= checkEnd) return;

    // Raw check section dump (only type bytes are XOR'd with xorByte, data is plaintext)
    {
        std::ostringstream oss;
        for (size_t i = checkStart; i < checkEnd; ++i) {
            if (i > checkStart) oss << ' ';
            oss << std::hex << std::uppercase << std::setfill('0') << std::setw(2)
                << (int)data[i];
        }
        LOG(INFO) << "[WARDEN]   check section (" << std::dec << (checkEnd - checkStart)
                  << " bytes, raw): " << oss.str();
    }

    // --- Try to discover types / learn sizes from this packet ---
    if (!warden_scan::HasTypeIDs()) {
        // Try binary scan first (if decompressed module available), then memory scan
        size_t moduleLen = 0;
        const uint8_t* moduleData = module_dump::GetDecompressedModule(moduleLen);
        if (moduleData && moduleLen > 0)
            warden_scan::ScanModuleBinary(moduleData, moduleLen);
        if (!warden_scan::HasTypeIDs())
            warden_scan::ScanAndExtractTypeIDs(); // fallback to memory scan
    }

    // Deterministic size assignment: tell the scanner how many strings
    // we have, then assign sizes to any new type IDs in this packet.
    warden_scan::SetStringCount(strings.size());
    if (!warden_scan::AssignTypeSizes(data, checkStart, checkEnd, xorByte)) {
        LOG(WARNING) << "[WARDEN]   AssignTypeSizes failed — structured parse "
                     << "will stop at first unknown type";
    }

    bool useDynamic = warden_scan::HasTypeIDs();

    // --- Structured parse ---
    static constexpr size_t kHookPatchSize = 8;
    int checkNum = 0;
    int memCheckCount = 0;
    bool truncated = false;
    pos = checkStart;

    while (pos < checkEnd && !truncated) {
        uint8_t realType = data[pos] ^ xorByte;
        pos++;
        checkNum++;

        bool validType;
        int dataSize;
        const char* typeName;

        if (useDynamic) {
            validType = warden_scan::IsValidType(realType);
            dataSize  = warden_scan::GetDataSize(realType);
            typeName  = warden_scan::GetTypeName(realType);
        } else {
            validType = IsKnownWardenCheckType(realType);
            dataSize  = static_cast<int>(WardenCheckDataSize(realType));
            typeName  = WardenCheckTypeToString(realType);
        }

        std::ostringstream info;
        info << "[WARDEN]   check#" << std::dec << checkNum
             << " type=0x" << std::hex << std::setfill('0') << std::setw(2)
             << (int)realType << " (" << typeName << ")";

        if (!validType) {
            info << " (unknown type, stopping parse)";
            LOG(INFO) << info.str();
            truncated = true;
            break;
        }

        if (dataSize < 0) {
            info << " (size not yet learned, stopping parse)";
            LOG(INFO) << info.str();
            truncated = true;
            break;
        }

        if (pos + static_cast<size_t>(dataSize) > checkEnd) {
            info << " (truncated, need " << std::dec << dataSize
                 << " bytes, have " << (checkEnd - pos) << ")";
            LOG(WARNING) << info.str();
            truncated = true;
            break;
        }

        // Extract and log fields based on data size
        if (dataSize == 6) {
            // MEM_CHECK: unk(1)+addr(4)+readLen(1)
            uint32_t addr;
            memcpy(&addr, data + pos + 1, 4);
            uint8_t readLen = data[pos + 5];
            info << " addr=0x" << std::hex << std::setfill('0') << std::setw(8) << addr
                 << " len=" << std::dec << (int)readLen;

            bool targetsFrameScript = (addr < kFrameScriptExecute + kHookPatchSize)
                                   && (kFrameScriptExecute < static_cast<uintptr_t>(addr) + readLen);
            bool targetsWarden = (addr < kWardenHandler + kHookPatchSize)
                              && (kWardenHandler < static_cast<uintptr_t>(addr) + readLen);

            if (targetsFrameScript)
                info << " *** TARGETS HOOK: FrameScript_Execute 0x"
                     << std::hex << kFrameScriptExecute << " ***";
            if (targetsWarden)
                info << " *** TARGETS HOOK: WardenHandler 0x"
                     << std::hex << kWardenHandler << " ***";

            memCheckCount++;
        } else if (dataSize == 31) {
            // PROC: seed(4)+SHA1(20)+modIdx(1)+procIdx(1)+addr(4)+readLen(1)
            uint8_t modIdx  = data[pos + 24];
            uint8_t procIdx = data[pos + 25];
            uint32_t addr;
            memcpy(&addr, data + pos + 26, 4);
            uint8_t readLen = data[pos + 30];
            info << " modIdx=" << (int)modIdx << " procIdx=" << (int)procIdx
                 << " addr=0x" << std::hex << std::setfill('0') << std::setw(8) << addr
                 << " len=" << std::dec << (int)readLen;
            if (modIdx < strings.size())
                info << " mod=\"" << strings[modIdx] << "\"";
            if (procIdx < strings.size())
                info << " proc=\"" << strings[procIdx] << "\"";
        } else if (dataSize == 29) {
            // PAGE: seed(4)+SHA1(20)+addr(4)+readLen(1)
            uint32_t addr;
            memcpy(&addr, data + pos + 24, 4);
            uint8_t readLen = data[pos + 28];
            info << " addr=0x" << std::hex << std::setfill('0') << std::setw(8) << addr
                 << " len=" << std::dec << (int)readLen;
        } else if (dataSize == 25) {
            // DRIVER: seed(4)+SHA1(20)+stringIndex(1)
            uint8_t strIdx = data[pos + 24];
            info << " strIdx=" << (int)strIdx;
            if (strIdx < strings.size())
                info << " name=\"" << strings[strIdx] << "\"";
        } else if (dataSize == 24) {
            // MODULE: seed(4)+SHA1(20)
            info << " seed+SHA1";
        } else if (dataSize == 1) {
            // MPQ/LUA: stringIndex(1)
            uint8_t strIdx = data[pos];
            info << " strIdx=" << (int)strIdx;
            if (strIdx < strings.size())
                info << " str=\"" << strings[strIdx] << "\"";
        }

        pos += static_cast<size_t>(dataSize);
        LOG(INFO) << info.str();
    }

    LOG(INFO) << "[WARDEN]   parsed " << std::dec << checkNum << " checks ("
              << memCheckCount << " MEM_CHECK)";

    if (truncated && pos < checkEnd) {
        LOG(WARNING) << "[WARDEN]   structured parse stopped at offset " << std::dec << pos
                     << " (" << (checkEnd - pos) << " bytes remaining)";
    }

    // Module-agnostic fallback: scan raw check data for our hook addresses.
    // This detects MEM_CHECK regardless of which Warden module is loaded.
    ScanForHookAddresses(data, checkStart, checkEnd);
}

// ---------------------------------------------------------------------------
// Post-handler: reads DECRYPTED data after original handler returns.
// At this point the original handler has decrypted the CDataStore buffer
// in-place and processed the packet.
// ---------------------------------------------------------------------------

static void __cdecl WardenPostHandlerImpl()
{
    void* pPacket = g_savedCDataStore;
    if (!pPacket)
        return;

    uintptr_t buffer;
    uint32_t size, readPos;
    if (!SafeReadCDataStore(pPacket, &buffer, &size, &readPos))
        return;

    // Use saved readPos from before the handler advanced it
    uint32_t payloadStart = g_savedReadPos;
    if (buffer < 0x10000 || size == 0 || size > 0xFFFF || payloadStart >= size)
        return;

    uint32_t payloadSize = size - payloadStart;
    uint8_t* data = (uint8_t*)buffer + payloadStart;

    // Copy payload into local buffer for safe processing
    size_t copyLen = (payloadSize < kMaxWardenPayload) ? payloadSize : kMaxWardenPayload;
    uint8_t localBuf[kMaxWardenPayload];
    if (!SafeReadBytes(data, localBuf, copyLen))
        return;

    uint8_t firstByte = localBuf[0];
    uint32_t pktNum = ++g_wardenPacketCount;

    // Record tick for CMSG_WARDEN latency measurement
    g_lastSmsgWardenTick = GetTickCount();

    // Hex dump (first N bytes)
    size_t dumpLen = (copyLen < kMaxDecryptedDump) ? copyLen : kMaxDecryptedDump;
    std::string hex = BytesToHex(localBuf, dumpLen);

    if (IsKnownWardenOpcode(firstByte)) {
        LOG(INFO) << "[WARDEN] pkt#" << std::dec << pktNum
                  << " opcode=0x" << std::hex << std::setfill('0') << std::setw(2)
                  << (int)firstByte
                  << " (" << WardenServerOpcodeToString(firstByte) << ")"
                  << " size=" << std::dec << payloadSize
                  << " data=[" << hex
                  << (payloadSize > dumpLen ? " ..." : "") << "]";

        if (firstByte == WARDEN_SMSG_CHEAT_CHECKS_REQUEST && copyLen >= 3)
            ParseCheatChecksRequest(localBuf, copyLen);

        // Module capture → dump to disk
        if (firstByte == WARDEN_SMSG_MODULE_USE) {
            warden_rc4_hook::Remove();
            warden_scan::Reset();
            warden_rc4::Reset();
            module_dump::Reset();
            module_dump::OnModuleUse(localBuf, copyLen);
            // Proactively try to load from disk cache
            if (copyLen >= 17)
                module_dump::TryLoadFromCache(localBuf + 1); // hash at offset 1
        }
        if (firstByte == WARDEN_SMSG_MODULE_CACHE)
            module_dump::OnModuleCache(localBuf, copyLen);
        if (firstByte == WARDEN_SMSG_MODULE_INITIALIZE) {
            module_dump::OnModuleInitialize(localBuf, copyLen);

            // Binary scan first (primary), memory scan as fallback
            size_t moduleLen = 0;
            const uint8_t* moduleData = module_dump::GetDecompressedModule(moduleLen);
            if (moduleData && moduleLen > 0) {
                warden_scan::LogModuleHeader(moduleData, moduleLen);
                if (!warden_scan::ScanModuleBinary(moduleData, moduleLen))
                    warden_scan::ScanAndExtractTypeIDs();
                warden_scan::FindModuleInMemory(moduleData, moduleLen);

                // Try to install internal RC4 hook (primary CMSG decryption)
                uintptr_t moduleAddr = warden_scan::GetModuleRuntimeAddress();
                size_t moduleRtSize  = warden_scan::GetModuleRuntimeSize();
                if (moduleAddr != 0 && moduleRtSize > 0)
                    warden_rc4_hook::Install(moduleAddr, moduleRtSize);
            } else {
                warden_scan::ScanAndExtractTypeIDs();
            }

            // S-box cloning as fallback (only if internal RC4 hook failed)
            if (!warden_rc4_hook::IsActive())
                warden_rc4::ScanForRC4States();
        }
    } else {
        LOG(WARNING) << "[WARDEN] pkt#" << std::dec << pktNum
                     << " first_byte=0x" << std::hex << std::setfill('0') << std::setw(2)
                     << (int)firstByte
                     << " (NOT a known Warden opcode — decryption may not be in-place)"
                     << " size=" << std::dec << payloadSize
                     << " data=[" << hex
                     << (payloadSize > dumpLen ? " ..." : "") << "]";
    }
}

// ---------------------------------------------------------------------------
// Wrapper: calls impl, then always re-enables hooks
// ---------------------------------------------------------------------------

static void __cdecl WardenPostHandler()
{
    WardenPostHandlerImpl();

    // Re-install hooks now that Warden handler has finished
    if (g_hooksDisabled) {
        MH_EnableHook(reinterpret_cast<LPVOID>(kFrameScriptExecute));
        MH_EnableHook(reinterpret_cast<LPVOID>(kWardenHandler));
        g_hooksDisabled = false;
    }

    // Clear ARC4 tracking flag
    g_insideWardenHandler = false;
}

// ---------------------------------------------------------------------------
// Naked detours
// ---------------------------------------------------------------------------

// Pre-hook: saves context, hijacks return address, jumps to original handler.
__declspec(naked) static void HookedWardenHandlerNaked()
{
    __asm {
        pushad
        pushfd

        mov eax, esp
        push eax
        call WardenPreHandler
        add esp, 4

        popfd
        popad

        jmp dword ptr [g_originalWardenHandler]
    }
}

// Post-hook: reached when original handler does `ret N`.
// Calls our C++ post-handler, then returns to the real caller.
__declspec(naked) static void WardenPostHandlerNaked()
{
    __asm {
        pushad
        pushfd

        call WardenPostHandler

        popfd
        popad

        jmp dword ptr [g_savedRetAddr]
    }
}

// ---------------------------------------------------------------------------
// ARC4::Process handler: captures plaintext before RC4 encrypt/decrypt
//
// ARC4::Process is __thiscall: ECX=ARC4*, stk1=data*, stk2=len
// After pushad+pushfd (36 bytes):
//   s[0]=EFLAGS s[1]=EDI s[2]=ESI s[3]=EBP s[4]=ESP_orig
//   s[5]=EBX s[6]=EDX s[7]=ECX(this) s[8]=EAX
//   s[9]=retaddr s[10]=data* s[11]=len
// ---------------------------------------------------------------------------

static void __cdecl ARC4ProcessHandler(uintptr_t savedEsp)
{
    if (!g_insideWardenHandler)
        return;

    // Safety: if flag is stale (>5 seconds), the handler likely crashed — clear it
    if (GetTickCount() - g_wardenHandlerStartTick > 5000) {
        LOG(WARNING) << "[ARC4] g_insideWardenHandler stale (>5s), force clearing";
        g_insideWardenHandler = false;
        return;
    }

    uint32_t* s = (uint32_t*)savedEsp;
    uint8_t* data = (uint8_t*)s[10];
    uint32_t len  = s[11];

    if (!data || len == 0 || len > kMaxWardenPayload)
        return;

    int callNum = ++g_wardenArc4CallNum;

    // Copy data BEFORE ARC4 modifies it (for encrypt, this is plaintext)
    uint8_t localBuf[kMaxWardenPayload];
    size_t copyLen = (len < kMaxWardenPayload) ? len : kMaxWardenPayload;
    if (!SafeReadBytes(data, localBuf, copyLen))
        return;

    size_t dumpLen = (copyLen < kMaxDecryptedDump) ? copyLen : kMaxDecryptedDump;
    std::string hex = BytesToHex(localBuf, dumpLen);

    const char* phase = (callNum == 1) ? "decrypt SMSG" : "encrypt CMSG";

    std::ostringstream oss;
    oss << "[ARC4] warden call#" << callNum
        << " len=" << len
        << " (" << phase << ")";

    // Log ARC4 this pointer on first call (diagnostic for S-box layout)
    if (callNum == 1) {
        oss << " this=0x" << std::hex << std::setfill('0') << std::setw(8)
            << s[7];
    }

    oss << " data=[" << hex;
    if (len > dumpLen)
        oss << " ...";
    oss << "]";

    LOG(INFO) << oss.str();
}

__declspec(naked) static void HookedARC4ProcessNaked()
{
    __asm {
        pushad
        pushfd

        mov eax, esp
        push eax
        call ARC4ProcessHandler
        add esp, 4

        popfd
        popad

        jmp dword ptr [g_originalARC4Process]
    }
}

// ---------------------------------------------------------------------------
// SendPacket handler: logs outgoing CMSG_WARDEN_DATA packets
//
// Stack after pushad+pushfd (same layout as WardenPreHandler):
//   s[0]=EFLAGS .. s[8]=EAX  s[9]=retaddr  s[10]=CDataStore*
//   (__thiscall: ECX=this, first stack arg = CDataStore*)
// ---------------------------------------------------------------------------

static void __cdecl SendPacketHandler(uintptr_t savedEsp)
{
    uint32_t* s = (uint32_t*)savedEsp;
    void* pPacket = (void*)s[10]; // CDataStore* — first stack arg

    if (!pPacket)
        return;

    uintptr_t buffer;
    uint32_t size, readPos;
    if (!SafeReadCDataStore(pPacket, &buffer, &size, &readPos))
        return;

    if (buffer < 0x10000 || size < 4)
        return;

    // Read opcode (first 4 bytes of CDataStore buffer)
    uint32_t opcode;
    if (!SafeReadBytes((const uint8_t*)buffer, (uint8_t*)&opcode, 4))
        return;

    // Diagnostic: log ALL outgoing packet opcodes
    LONG diagNum = InterlockedIncrement(&g_sendPacketDiagCount);
    LOG(INFO) << "[SEND] #" << std::dec << diagNum
              << " opcode=0x" << std::hex << std::setfill('0') << std::setw(4)
              << opcode << " size=" << std::dec << size;

    // Fast path: skip non-warden packets
    if (opcode != kCmsgWardenDataOpcode)
        return;

    LONG pktNum = InterlockedIncrement(&g_cmsgWardenCount);
    uint32_t payloadSize = size - 4;

    // Latency since last SMSG_WARDEN_DATA
    DWORD latency = 0;
    DWORD lastTick = g_lastSmsgWardenTick;
    if (lastTick != 0)
        latency = GetTickCount() - lastTick;

    // Copy payload for hex dump
    size_t copyLen = (payloadSize < kMaxWardenPayload) ? payloadSize : kMaxWardenPayload;
    uint8_t localBuf[kMaxWardenPayload];
    if (copyLen > 0 && !SafeReadBytes((const uint8_t*)(buffer + 4), localBuf, copyLen))
        return;

    size_t dumpLen = (copyLen < kMaxDecryptedDump) ? copyLen : kMaxDecryptedDump;
    std::string hex = (dumpLen > 0) ? BytesToHex(localBuf, dumpLen) : std::string();

    std::ostringstream oss;
    oss << "[CMSG_WARDEN] pkt#" << std::dec << pktNum
        << " opcode=0x" << std::hex << std::setfill('0') << std::setw(4)
        << opcode << " total=" << std::dec << size
        << " payload=" << payloadSize << " bytes";
    if (latency > 0)
        oss << " latency=" << latency << "ms";

    // Try to decrypt CMSG payload:
    // 1. Internal RC4 hook (captures plaintext before encryption — no race condition)
    // 2. S-box cloning fallback (warden_rc4::DecryptCmsg)
    uint8_t plaintext[kMaxWardenPayload];
    size_t plainLen = 0;
    bool decrypted = false;

    if (warden_rc4_hook::IsActive())
        decrypted = warden_rc4_hook::ConsumePlaintext(plaintext, sizeof(plaintext), &plainLen);

    if (!decrypted && copyLen > 0) {
        decrypted = warden_rc4::DecryptCmsg(localBuf, copyLen, plaintext, sizeof(plaintext));
        if (decrypted)
            plainLen = copyLen;
    }

    if (decrypted && plainLen > 0) {
        size_t ptDump = (plainLen < kMaxDecryptedDump) ? plainLen : kMaxDecryptedDump;
        std::string ptHex = BytesToHex(plaintext, ptDump);
        oss << "\n  decrypted=[" << ptHex;
        if (plainLen > ptDump)
            oss << " ...";
        oss << "]";

        uint8_t clientOp = plaintext[0];
        oss << " warden_op=0x" << std::hex << std::setfill('0') << std::setw(2)
            << (int)clientOp << " (" << WardenClientOpcodeToString(clientOp) << ")";
        if (warden_rc4_hook::IsActive())
            oss << " [rc4_hook]";
    } else {
        oss << " data=[" << hex;
        if (payloadSize > dumpLen)
            oss << " ...";
        oss << "] (encrypted)";
    }

    LOG(INFO) << oss.str();
}

__declspec(naked) static void HookedSendPacketNaked()
{
    __asm {
        pushad
        pushfd

        mov eax, esp
        push eax
        call SendPacketHandler
        add esp, 4

        popfd
        popad

        jmp dword ptr [g_originalSendPacket]
    }
}

// ===========================================================================
// Public API
// ===========================================================================

namespace hooks {

bool Initialize()
{
    MH_STATUS status = MH_Initialize();
    if (status != MH_OK) {
        LOG(ERROR) << "MH_Initialize failed: " << MH_StatusToString(status);
        return false;
    }

    // --- FrameScript_Execute hook ---
    status = MH_CreateHook(
        reinterpret_cast<LPVOID>(kFrameScriptExecute),
        reinterpret_cast<LPVOID>(&HookedFrameScriptExecute),
        reinterpret_cast<LPVOID*>(&g_originalFrameScriptExecute));

    if (status != MH_OK) {
        LOG(ERROR) << "MH_CreateHook(FrameScript_Execute) failed: "
                   << MH_StatusToString(status);
        MH_Uninitialize();
        return false;
    }

    status = MH_EnableHook(reinterpret_cast<LPVOID>(kFrameScriptExecute));
    if (status != MH_OK) {
        LOG(ERROR) << "MH_EnableHook(FrameScript_Execute) failed: "
                   << MH_StatusToString(status);
        MH_Uninitialize();
        return false;
    }

    LOG(INFO) << "Hook installed: FrameScript_Execute @ 0x"
              << std::hex << kFrameScriptExecute;

    // --- SMSG_WARDEN_DATA hook ---
    g_wardenPostHandlerAddr = (void*)&WardenPostHandlerNaked;

    status = MH_CreateHook(
        reinterpret_cast<LPVOID>(kWardenHandler),
        reinterpret_cast<LPVOID>(&HookedWardenHandlerNaked),
        reinterpret_cast<LPVOID*>(&g_originalWardenHandler));

    if (status != MH_OK) {
        LOG(ERROR) << "MH_CreateHook(WardenHandler) failed: "
                   << MH_StatusToString(status);
        return true;
    }

    status = MH_EnableHook(reinterpret_cast<LPVOID>(kWardenHandler));
    if (status != MH_OK) {
        LOG(ERROR) << "MH_EnableHook(WardenHandler) failed: "
                   << MH_StatusToString(status);
        return true;
    }

    LOG(INFO) << "Hook installed: SMSG_WARDEN_DATA @ 0x"
              << std::hex << kWardenHandler;

    // --- ClientServices::SendPacket hook ---
    status = MH_CreateHook(
        reinterpret_cast<LPVOID>(kSendPacket),
        reinterpret_cast<LPVOID>(&HookedSendPacketNaked),
        reinterpret_cast<LPVOID*>(&g_originalSendPacket));

    if (status != MH_OK) {
        LOG(ERROR) << "MH_CreateHook(SendPacket) failed: "
                   << MH_StatusToString(status);
    } else {
        status = MH_EnableHook(reinterpret_cast<LPVOID>(kSendPacket));
        if (status != MH_OK) {
            LOG(ERROR) << "MH_EnableHook(SendPacket) failed: "
                       << MH_StatusToString(status);
        } else {
            LOG(INFO) << "Hook installed: SendPacket @ 0x"
                      << std::hex << kSendPacket;
        }
    }

    // --- ARC4::Process hook (non-fatal) ---
    status = MH_CreateHook(
        reinterpret_cast<LPVOID>(kARC4Process),
        reinterpret_cast<LPVOID>(&HookedARC4ProcessNaked),
        reinterpret_cast<LPVOID*>(&g_originalARC4Process));

    if (status != MH_OK) {
        LOG(ERROR) << "MH_CreateHook(ARC4::Process) failed: "
                   << MH_StatusToString(status);
    } else {
        status = MH_EnableHook(reinterpret_cast<LPVOID>(kARC4Process));
        if (status != MH_OK) {
            LOG(ERROR) << "MH_EnableHook(ARC4::Process) failed: "
                       << MH_StatusToString(status);
        } else {
            LOG(INFO) << "Hook installed: ARC4::Process @ 0x"
                      << std::hex << kARC4Process;
        }
    }

    // Try to extract Warden module type IDs at startup
    // (module may already be loaded before DLL injection)
    warden_scan::ScanAndExtractTypeIDs();

    return true;
}

void Shutdown()
{
    warden_rc4_hook::Remove();
    warden_rc4_hook::Cleanup();

    MH_DisableHook(reinterpret_cast<LPVOID>(kARC4Process));
    MH_DisableHook(reinterpret_cast<LPVOID>(kSendPacket));
    MH_DisableHook(reinterpret_cast<LPVOID>(kWardenHandler));
    MH_DisableHook(reinterpret_cast<LPVOID>(kFrameScriptExecute));

    MH_STATUS status = MH_Uninitialize();
    if (status != MH_OK) {
        LOG(WARNING) << "MH_Uninitialize failed: " << MH_StatusToString(status);
    } else {
        LOG(INFO) << "All hooks removed (SMSG warden packets: "
                  << std::dec << g_wardenPacketCount
                  << ", CMSG warden packets: " << g_cmsgWardenCount << ")";
    }
}

} // namespace hooks
