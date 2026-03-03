# Warden anti-cheat internals in WoW 3.3.5a

Blizzard's Warden is a dynamically-loaded, RSA-signed, custom-format binary module (~48 KB) that performs periodic memory, module, file, and timing integrity scans on the WoW client — but **all enforcement decisions (kick/ban) are made server-side**, not within the module itself. The module collects scan results and returns them over an RC4-encrypted channel; it never directly triggers a disconnect or displays the "Unauthorized software" popup. This architecture means the most effective neutralization strategy targets the module's scan data-collection routines rather than any client-side enforcement path. Below is a complete technical breakdown of every layer.

---

## Module binary format: a custom PE-like container

The Warden module is not a standard PE/DLL. It arrives encrypted (RC4 with a 16-byte key from the server), RSA-signed (256-byte signature verified against `SHA1(data + "MAIEV.MOD")` with exponent **65537** and a 256-byte public modulus hardcoded in the client), and zlib-compressed. After decompression, the binary begins with a **40-byte header** at offset 0x00 containing 10 DWORD fields:

```c
struct WardenModuleHeader {          // 40 bytes (0x28)
    DWORD moduleSize;                // 0x00  Total prepared image size (VirtualAlloc size)
    DWORD unknown_04;                // 0x04  Reserved
    DWORD relocOffset;               // 0x08  Offset to relocation stream
    DWORD relocCount;                // 0x0C  Number of relocation fixups
    DWORD exportTableOffset;         // 0x10  Offset to DWORD array of export RVAs
    DWORD exportCount;               // 0x14  Number of exports
    DWORD baseIndex;                 // 0x18  Base ordinal (index = ordinal − baseIndex)
    DWORD importTableOffset;         // 0x1C  Offset to import library descriptors
    DWORD importLibCount;            // 0x20  Number of imported DLLs
    DWORD sectionDescCount;          // 0x24  Section descriptor count
};
```

**Section descriptors** begin immediately at offset **0x28**, each **12 bytes**: a `virtualAddress` (destination RVA), `virtualSize`, and `characteristics` (RWX flags). After the descriptors, packed section data uses an alternating run-length scheme — read a 16-bit length, copy that many bytes, read another length, skip that many bytes (zero-fill), repeat — until `destLocation >= moduleSize`. This is not zlib; it is a simple sparse-section packing applied after the outer zlib decompression.

**Relocations** use a compact delta-encoded stream at `relocOffset`. Each entry is either 2 bytes (high bit clear → 16-bit big-endian delta added to running offset) or 4 bytes (high bit set → 31-bit absolute offset). Each fixup adds the loaded base address to the DWORD at the target location — identical in concept to `IMAGE_REL_BASED_HIGHLOW`.

**The import table** at `importTableOffset` contains `importLibCount` 8-byte entries: `{DWORD libraryNameOffset, DWORD functionsArrayOffset}`. The functions array is a null-terminated sequence of DWORDs — positive values are offsets to function name strings (import by name), negative values (high bit set) are ordinal imports. The loader calls `LoadLibrary` / `GetProcAddress` and overwrites each entry in-place with the resolved address. Commonly imported DLLs: **kernel32.dll** (VirtualAlloc, VirtualQuery, VirtualProtect, GetModuleHandle, CreateToolhelp32Snapshot, Module32First/Next), **ntdll.dll**, and **user32.dll**.

**The export table** at `exportTableOffset` is a flat DWORD array of RVAs. Lookup formula: `funcAddr = moduleBase + exportTable[ordinal − baseIndex]`.

The known stable signature **`56 57 FC 8B 54 24 14 8B 74 24 10 8B 44 24 0C 8B CA 8B F8 C1 E9 02 74 02 F3 A5`** corresponds to the module's internal memcpy-like scan function — a `push esi / push edi / cld` prologue followed by `rep movsd` for DWORD-aligned copy and `rep movsb` for remainder bytes. This function is what Warden uses to read client memory during integrity checks. It has remained stable across module versions despite obfuscation of surrounding code, making it the primary hooking target.

---

## Initialization, callbacks, and the module export interface

After the module is loaded via `VirtualAlloc(NULL, moduleSize, MEM_COMMIT, PAGE_EXECUTE_READWRITE)`, sections are unpacked, relocations applied, and imports resolved. The client then calls the **Initialize export** (ordinal 1, `__fastcall` convention — callback table pointer in ECX). This receives a pointer to a **7-function callback table** provided by the WoW client:

```c
struct FuncList {                    // 28 bytes, all __stdcall
    fnSendPacket      fpSendPacket;       // 0x00  Send warden response to server
    fnCheckModule     fpCheckModule;      // 0x04  Check if sub-module is cached
    fnLoadModule      fpLoadModule;       // 0x08  Load a sub-module
    fnAllocateMemory  fpAllocateMemory;   // 0x0C  Heap alloc
    fnReleaseMemory   fpReleaseMemory;    // 0x10  Heap free
    fnSetRC4Data      fpSetRC4Data;       // 0x14  Persist RC4 state
    fnGetRC4Data      fpGetRC4Data;       // 0x18  Retrieve RC4 state
};
```

Initialize returns a pointer-to-pointer to a **4-function export table** (all `__thiscall`, ECX = ppFuncList):

```c
struct WardenFuncList {
    fnGenerateRC4Keys  fpGenerateRC4Keys;  // 0x00  Process seed → new RC4 keys
    fnUnloadModule     fpUnload;           // 0x04  Teardown
    fnPacketHandler    fpPacketHandler;    // 0x08  Main scan request processor
    fnTick             fpTick;             // 0x0C  Periodic tick (timing delta)
};
```

The **PacketHandler** (offset 0x08) is the core workhorse — it receives decrypted `WARDEN_SMSG_CHEAT_CHECKS_REQUEST` payloads, dispatches each scan type through an internal jump table, and packages results for return via `fpSendPacket`. The **GenerateRC4Keys** export processes the 16-byte seed from `WARDEN_SMSG_HASH_REQUEST` (sub-opcode 5), generating the SHA1 response and new session-specific RC4 keys.

A separate `WARDEN_SMSG_MODULE_INITIALIZE` packet (sub-opcode 3, a 57-byte packed struct) passes additional WoW client function pointers directly into the module's internal state. For **3.3.5a build 12340**, these are:

- **SFileOpenFile**: `0x002485F0` — MPQ archive access
- **SFileGetFileSize**: `0x002487F0`
- **SFileReadFile**: `0x00248460`
- **SFileCloseFile**: `0x00248730`
- **FrameScript::GetText**: `0x00419D40` — Lua variable reading
- **PerformanceCounter**: `0x0046AE20` — Timing checks

These function pointers enable the module to read MPQ files (for MPQ_CHECK), evaluate Lua state (for LUA_EVAL_CHECK), and perform timing measurements without needing to resolve these game-specific symbols itself.

---

## Nine scan types and their wire formats

All Warden communication rides inside **SMSG_WARDEN_DATA** (opcode **0x02E6**, server→client) and **CMSG_WARDEN_DATA** (opcode **0x02E7**, client→server), with payloads RC4-encrypted on top of standard world packet header encryption. The sub-opcode system within these packets is:

| Sub-opcode | Direction | Purpose |
|---|---|---|
| 0 | S→C | MODULE_USE — send module MD5 + RC4 key + size |
| 1 | S→C | MODULE_CACHE — transfer module data (≤500 byte chunks) |
| 2 | S→C / C→S | CHEAT_CHECKS_REQUEST / CHEAT_CHECKS_RESULT |
| 3 | S→C | MODULE_INITIALIZE — pass function pointers |
| 5 | S→C / C→S | HASH_REQUEST / HASH_RESULT — key exchange |

The check request (sub-opcode 2) begins with a **string table** (sequential null-terminated strings referenced by index), followed by scan entries. **TIMING_CHECK (0x57)** is always first. The packet terminates with an XOR checksum byte of all type IDs. The nine scan types with their request/response formats:

**MEM_CHECK (0xF3)** — the primary code-patch detector. Request: `{byte 0x00, byte moduleNameIndex, uint32 offset, byte readLength}`. The module reads `readLength` bytes at `moduleBase + offset` (empty module name = WoW.exe). Response: `{byte result}` where 0x00 means match; on mismatch, followed by `{byte length, byte[length] actualData}`. **The server compares the returned bytes** against expected values stored in its `warden_checks` database.

**PAGE_CHECK_A (0xB2) / PAGE_CHECK_B (0xBF)** — SHA1-hash memory integrity checks. Request: `{uint32 seed, byte[20] expectedSHA1, uint32 address, byte length}`. PAGE_CHECK_A scans all pages; PAGE_CHECK_B only scans pages beginning with MZ+PE headers (targeting loaded executables). Response: single byte, **0xE9 = pass** (hash matches), anything else = fail. The comparison happens client-side, making these more bandwidth-efficient than MEM_CHECK but returning less diagnostic data.

**MODULE_CHECK (0xD9)** — detects injected DLLs. Uses HMAC-SHA1 of the module name. Response: **0xE9 = module NOT found** (pass).

**DRIVER_CHECK (0x71)** — detects loaded kernel drivers by name hash. Same 0xE9 pass convention.

**PROC_CHECK (0x7E)** — verifies specific API function prologues haven't been detoured. Reads bytes at a function entry point.

**MPQ_CHECK (0x98)** — SHA1-hashes MPQ game data files to detect modifications. Response: 20-byte SHA1.

**LUA_EVAL_CHECK (0x8B)** — evaluates Lua code in the client. In 3.3.5a, uses `FrameScript::GetText` (not Execute), running **without elevated privileges** (state parameter = 0). Results are sent back via addon message channel.

**TIMING_CHECK (0x57)** — verifies `GetTickCount()` isn't detoured. Response: `{byte result, uint32 clientTicks}`.

The response packet structure is: `{byte subOpcode(0x02), uint16 length, uint32 checksum}` followed by per-check results in request order. The checksum is the first 4 bytes of `SHA1(responseData)`.

---

## Known scanned addresses in 3.3.5a build 12340

Warden's `warden_checks` database for 3.3.5a targets these commonly-patched locations:

| Address | Len | Target | Category |
|---|---|---|---|
| **0x00819210** | 1 | FrameScript__Execute | Lua unlocker detection |
| **0x0084F860** | — | FrameScript_Load | Lua unlocker detection |
| **0x005120E0** | — | LoadScriptFunctions | Lua unlocker detection |
| 0x0049DBB2 | 7 | Protected Lua Func Check | Lua protection |
| 0x004AA9C2 | 5 | FrameXML Signature Check | UI integrity |
| 0x008C8398 | 8 | Max Wall Climb angle | Physics hack |
| 0x008C845C | 8 | Gravity constant | Physics hack |
| 0x008F7AC8 | 8 | Jump Velocity | Physics hack |
| 0x005CDC20 | 6 | Falling Check | Fly hack |
| 0x00420541 | 10 | WS2_32.Send | Network hook |
| 0x0048D4A0 | 12 | AddChatMessage | Chat hook |
| 0x00490430 | 12 | SendChatMessage | Chat hook |

These addresses are MEM_CHECK targets — the server requests the exact bytes and compares them against known-good values. **Any byte modification at these addresses will be detected** unless the scan result is intercepted.

---

## Detection signaling is entirely server-side

A critical architectural insight: **the Warden module never kicks, bans, or displays error messages**. It is a pure data-collection agent. The flow is:

1. Module executes scans via its PacketHandler jump table
2. Results are sent to the server via `fpSendPacket` callback → `CMSG_WARDEN_DATA`
3. Server compares results against expected values in `warden_checks` DB
4. On mismatch, server applies the configured action (`Warden.ClientCheckFailAction`: 0=log, 1=kick, 2=ban)
5. Server sends a disconnect packet with a reason code
6. Client's standard disconnect handler displays the localized error ("Unauthorized software or modifications detected")

The "Unauthorized software" popup is **not triggered through `FrameScript_Execute` at 0x00819210** by the Warden module. That address is a *scan target* (Warden checks whether it's been hooked), not part of the detection signaling path. The popup comes from WoW's standard disconnect UI handling — likely `StaticPopup_Show` invoked by the network layer's disconnect reason handler. **The disconnect and message display are coupled** — both originate from the server's disconnect packet. There is no separate client-side enforcement path to suppress independently.

If the client fails to respond to a scan request within the configured timeout (default **600 seconds**), the server disconnects anyway. On Blizzard retail, detections were historically queued for delayed **ban waves** rather than triggering immediate kicks.

---

## RC4 encryption and key lifecycle

The Warden encryption channel operates as a separate RC4 layer inside the already-encrypted world packets. Key lifecycle:

**Phase 1 — Session-derived keys**: After authentication, 16-byte input/output RC4 keys are derived from the 40-byte SRP6 session key `K` via `SHA1Randx` (split K into two 20-byte halves, iteratively SHA1-hash each to generate key material).

**Phase 2 — Module-specific keys**: After module load, the server sends `WARDEN_SMSG_HASH_REQUEST` with a 16-byte seed. The module's `GenerateRC4Keys` export processes this seed and returns a 20-byte SHA1 hash. The server verifies this against `Module.ClientKeySeedHash`. On success, **both sides rotate** to module-specific RC4 keys (`Module.ClientKeySeed` for client→server, `Module.ServerKeySeed` for server→client). From this point, all Warden traffic uses the new keys.

Since RC4 is a stream cipher, **packet ordering is critical** — any dropped, duplicated, or reordered Warden packet desynchronizes the cipher state permanently, causing all subsequent packets to fail decryption.

---

## Evasion: hooking the scan function's output buffer

The most effective and widely-documented technique targets the module's internal memcpy/scan function (the stable signature pattern). Rather than restoring original bytes before scans, **the superior approach modifies the output buffer after the scan executes**:

1. **Detect module load**: Hook `VirtualAlloc` inside `LoadWardenModule` (at **0x00872350** in 3.3.5a). Capture allocations >0x2000 bytes with `PAGE_EXECUTE_READWRITE` from the Warden loader address range. This gives you the module's base address and size.

2. **Pattern-scan for the scan function**: Search the allocated region for `56 57 FC 8B 54 24 14` or the shorter `74 02 F3 A5 B1 03 23 CA` pattern. This locates the memcpy routine Warden uses for all memory reads.

3. **Install an inline detour**: Write a `JMP` (0xE9) at the function entry to your hook. In your detour, call the original function (let Warden read real memory), then check if the scanned source address overlaps any of your patches. If so, **overwrite the destination buffer** with the original clean bytes.

A critical caveat: Warden validates against **runtime bytes**, not on-disk bytes. The `.data` section contains values modified at load time. Only `.text` and `.rdata` bytes match the on-disk binary. For data-section patches, you must capture the original runtime bytes *before* applying your modifications and use those as the "clean" reference. Also, Warden can issue **overlapping address scans** (e.g., `0x8B5BEF` length 9 and `0x8B5BF6` length 5), which must be treated as a single buffer to avoid inconsistencies.

As OwnedCore researcher Cypher confirmed: **"Two simple function hooks are all that's needed"** — no DLL cloaker, Windows API hooks, or ring-0 driver required. This approach was the dominant bypass strategy throughout the WotLK era.

---

## Alternative evasion approaches and their tradeoffs

**VEH / hardware breakpoint hooks** avoid modifying any bytes in the module or client, making them invisible to CRC/SHA1 integrity checks. Set a hardware breakpoint (DR0–DR3) on the scan function entry; a Vectored Exception Handler catches the single-step exception and redirects execution. Community consensus for the 3.3.5a era: "Warden does not care about debug registers." The downside is exception-handling overhead and the 4-breakpoint hardware limit.

**Adjacent-code patching** exploits Warden's finite scan list. For example, address `0x494A57` (Lua protection check) is scanned, but a `test eax, eax` instruction immediately before it achieves the same functional bypass when changed to `xor eax, eax` — and this adjacent location is not in the scan database. This is fragile (new scan entries can be added), but effective when combined with scan-function hooking as a belt-and-suspenders approach.

**Timing-based restore/re-patch** (remove patches before scans, reapply after) is inferior to buffer replacement. It introduces race conditions, leaves a window where hacks are inactive, and Warden scans multiple addresses per cycle at ~60-second intervals. The buffer-replacement method avoids all these issues.

**Manual DLL mapping + PEB unlinking** prevents MODULE_CHECK from detecting injected DLLs. Load your code manually (emulate the Windows loader) so no entry appears in the PEB's `InLoadOrderModuleList`, `InMemoryOrderModuleList`, or `InInitializationOrderModuleList`. Combined with hooking `Module32First/Next` and `VirtualQuery`, this makes your DLL invisible to Warden's enumeration.

**Module emulation via CPU emulation** (e.g., Unicorn Engine) runs the entire Warden module in a sandbox where all memory reads are interceptable. The WoWee project demonstrates this approach. Extremely robust but very complex to implement.

---

## Does Warden check its own integrity?

PAGE_CHECK computes SHA1 hashes of memory pages. If you inline-hook the Warden scan function, the page containing your hook has different bytes, and a PAGE_CHECK targeting that page would theoretically detect it. However, **community consensus for the WotLK era is that Warden was not aggressive about self-integrity checking**. The module's primary defense against tampering was code obfuscation (polymorphic function ordering, wrapper functions added/removed between versions), not runtime self-verification.

If self-checks are a concern, three mitigations exist. First, hook both MEM_CHECK and PAGE_CHECK handlers — for PAGE_CHECK, if the target overlaps your hooks, compute the hash from original bytes. Second, keep an unmodified copy of the Warden module and redirect self-targeted scans to it. Third, use VEH/hardware breakpoints instead of inline hooks — **zero bytes modified means zero bytes to detect**.

The scan function's byte pattern has remained stable despite obfuscation of the surrounding module. As documented in the HackMag analysis: "The detected code is not subject to polymorphic changes, unlike the rest of the module; moreover, it has been changed only once over recent years."

---

## Conclusion

Warden in WoW 3.3.5a is architecturally a **server-validated telemetry collector**, not a client-side enforcer. The module's custom binary format, RSA signing, and RC4 encryption protect it from replacement or modification at rest, but at runtime its scan logic funnels through a single, pattern-scannable memcpy function whose output buffer can be intercepted and cleaned. The most reliable neutralization strategy combines three elements: detecting module loads via `VirtualAlloc` hooking, pattern-scanning for the stable `56 57 FC 8B 54 24 14` signature, and installing an inline detour that substitutes clean bytes in the scan output buffer for any address overlapping a client patch. This approach handles MEM_CHECK, PAGE_CHECK, and self-integrity checks simultaneously, requires no kernel-mode component, and was proven effective throughout the WotLK era. The critical implementation detail is using captured **runtime** original bytes (not on-disk bytes) as the clean reference, particularly for `.data` section addresses where load-time initialization changes values before your patches are applied.