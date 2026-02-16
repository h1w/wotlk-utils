# Warden anti-cheat in WoW 3.3.5a: a deep technical teardown

Warden modules in World of Warcraft 3.3.5a (build 12340) use a **custom Blizzard binary format — not PE** — with a 40-byte header, RLE-packed sections, delta-encoded relocations, and a name-based import resolver. Each module is ~28–30 KB decompressed, loaded via `VirtualAlloc` with `PAGE_EXECUTE_READWRITE` into a single contiguous allocation, and initialized through a `__fastcall` entry point that exchanges function pointer tables between module and host. Server-side emulators (TrinityCore, AzerothCore, MaNGOS) hardcode check type IDs from a single reverse-engineered module, but real Blizzard modules use **per-module check type byte values**, making static analysis of any one module insufficient for universal Warden understanding. The three specific module hashes queried (7C4ABC97, 9A95D199, DA3BF29E) do not appear in any public repository or writeup, suggesting they originate from private reverse engineering work.

---

## The module binary is a custom format with a 40-byte header

After RC4 decryption and zlib decompression, the Warden module blob is definitively **not a PE file**. It lacks MZ/PE signatures and uses a completely custom format documented by Ron Bowes (SkullSecurity) and independently confirmed by multiple emulator implementations. The decompressed blob starts with a **40-byte (0x28) fixed header**, followed by section descriptors and packed code/data:

| Offset | Size | Field | Purpose |
|--------|------|-------|---------|
| 0x00 | 4 | `dwModuleSize` | Total final runtime size after section expansion |
| 0x04 | 4 | Reserved | Unknown/unused |
| 0x08 | 4 | `dwRelocDataOffset` | Byte offset to relocation entries |
| 0x0C | 4 | `dwRelocCount` | Number of relocation fixups |
| 0x10 | 4 | `dwExportTableOffset` | Offset to function pointer/export table |
| 0x14 | 4 | `dwExportCount` | Number of exported functions |
| 0x18 | 4 | `dwBaseIndex` | Index used for entry point calculation |
| 0x1C | 4 | `dwImportTableOffset` | Offset to import library descriptors |
| 0x20 | 4 | `dwImportLibraryCount` | Number of import libraries |
| 0x24 | 4 | `dwSectionDescCount` | Number of 12-byte section descriptors |

Section descriptors begin at offset 0x28, each 12 bytes. The packed section data follows at offset `0x28 + (dwSectionDescCount × 12)`. Sections are stored using an **alternating copy/skip RLE scheme**: a 2-byte little-endian length prefix followed by either raw data to copy or a gap count to skip (zero-filled, analogous to `.bss`). The loader toggles between copy and skip modes with each length prefix.

The pre-decompression pipeline is: **RC4 decrypt → verify RSA-2048 signature → zlib inflate**. The encrypted blob's structure after RC4 decryption is `[4-byte uncompressed length][compressed data][4-byte "NGIS" marker][256-byte RSA signature]`. The RSA signature covers `SHA1(data + "MAIEV.MOD")`, padded with `0xBB` bytes to 256 bytes, using exponent **65537** and a fixed 256-byte modulus shared across WoW versions 1.12.1 through 3.3.5a.

---

## PrepareModule: relocations, imports, and entry point initialization

The `PrepareModule` function (embedded in WoW.exe) acts as a custom loader performing three critical fixup passes after section unpacking.

**Relocations** use a compact delta-encoded format. Each entry is either 2 bytes (relative delta, common case) or 4 bytes (absolute offset, signaled by the high bit being set). For 2-byte entries, the destination offset advances by `(byte[0] << 8) | byte[1]`. For 4-byte entries, the absolute offset is `((byte[0] & 0x7F) << 24) | (byte[1] << 16) | (byte[2] << 8) | byte[3]`. At each relocation target, the module's actual base address is added to the existing DWORD value — a straightforward base relocation identical in concept to PE `.reloc` processing.

**Import resolution** iterates over `dwImportLibraryCount` entries, each an 8-byte descriptor: 4 bytes for the offset to a null-terminated library name string, 4 bytes for the offset to a null-terminated function pointer slot array. Each slot initially contains either a positive offset (pointing to a function name string for resolution via `GetProcAddress`) or a negative value (import by ordinal, masked with `0x7FFFFFFF`). After resolution, slots are overwritten with actual function addresses. The list terminates with a zero DWORD. Typical imported libraries are `kernel32.dll` and `user32.dll`.

**Entry point calculation** uses the export table:

```c
DWORD EBP = header[0x18];           // base index
DWORD EDX = 1 - EBP;               // adjusted export index
DWORD ECX = header[0x10];           // export table offset
DWORD entryRVA = *(DWORD*)(module + ECX + EDX * 4);
fnInitializeModule init = (fnInitializeModule)(module + entryRVA);
```

The init function uses **`__fastcall` convention** and receives a pointer to a 7-function callback table (28 bytes) provided by the host: `SendPacket`, `CheckModule`, `LoadModule`, `AllocateMemory`, `ReleaseMemory`, `SetRC4Data`, `GetRC4Data`. It returns a pointer-to-pointer to a 4-function export table: `GenerateRC4Keys`, `UnloadModule`, `PacketHandler`, `Tick`. All exported functions use `__thiscall` convention with the first parameter being the `WardenFuncList**` self-pointer.

---

## Check type dispatch: sparse values favor cmp/je chains over jump tables

The check type dispatch mechanism inside Warden modules has not been publicly documented at the assembly level for any specific module. However, the architectural constraints strongly constrain the possibilities.

**The nine check types for the "standard" emulator module** use these byte values: `TIMING_CHECK=0x57`, `DRIVER_CHECK=0x71`, `PROC_CHECK=0x7E`, `LUA_STR_CHECK=0x8B`, `MPQ_CHECK=0x98`, `PAGE_CHECK_A=0xB2`, `PAGE_CHECK_B=0xBF`, `MODULE_CHECK=0xD9`, `MEM_CHECK=0xF3`. These span a **156-byte range (0x57–0xF3)** with only 9 active entries — a density of ~5.8%. A 256-entry jump table (`jmp dword ptr [eax*4 + table_base]`) would waste **1,024 bytes** for 9 entries, representing ~3.4% of a 30KB module. A sequential `cmp al, imm8` / `je target` chain requires only ~54 bytes (6 bytes per comparison × 9 types) and is the natural compiler output for a `switch` on sparse values — making it the **most probable dispatch pattern for module 7C4ABC97**.

For **module 9A95D199** (28,876 bytes), where scanning for `cmp al, imm8` clusters fails, several alternative patterns should be investigated:

- **`sub al, imm8` / `jz` chains**: The compiler may subtract the check type constant and test for zero instead of comparing directly. This changes the opcode pattern from `3C xx` to `2C xx 74 yy`.
- **Compact function pointer table**: A small 9-entry lookup table indexed by a hash or remapping of the check type byte, with dispatch via `call dword ptr [edx + eax*4]`.
- **Nested binary comparisons**: The compiler may split the 9 values using binary search-style `cmp`/`jb`/`ja` branching, halving the search space at each level.
- **XOR/rotate obfuscation**: The type byte may be transformed (e.g., `xor al, key; ror al, N`) before comparison, hiding the literal check type values.
- **Indirect `movzx` + table lookup**: `movzx eax, al; movzx eax, byte ptr [remap_table + eax]; jmp dword ptr [handler_table + eax*4]` — a 256-byte remap table compresses the sparse range to contiguous indices.

**Critically, different Warden modules use different check type byte values for the same logical checks.** This is confirmed by OwnedCore researchers: "the CheckTypes change depending on the warden module you have loaded into the game." Module DA3BF29E reportedly uses `TIMING=0x74` and `LUA=0x70` — completely different from the "standard" `0x57` and `0x8B`. This per-module variation serves as an additional obfuscation layer and explains why the emulator-hardcoded values only work with one specific module.

---

## Server-side emulators hardcode one module's check type enum

TrinityCore, AzerothCore, and MaNGOS all define identical check type constants in `Warden.h`:

```cpp
enum WardenCheckType {
    MEM_CHECK       = 0xF3,  // Read memory bytes at offset
    PAGE_CHECK_A    = 0xB2,  // SHA1 scan all memory pages
    PAGE_CHECK_B    = 0xBF,  // SHA1 scan MZ+PE header pages only
    MPQ_CHECK       = 0x98,  // Verify MPQ file integrity
    LUA_STR_CHECK   = 0x8B,  // Execute/check Lua string (AzerothCore: LUA_EVAL_CHECK)
    DRIVER_CHECK    = 0x71,  // Check if driver is loaded
    TIMING_CHECK    = 0x57,  // Verify GetTickCount() not detoured
    PROC_CHECK      = 0x7E,  // Check if exported proc is detoured
    MODULE_CHECK    = 0xD9,  // Detect injected module
};
```

The **CHEAT_CHECKS_REQUEST packet** (inner opcode 0x02) is built by `WardenWin::RequestChecks()`. Non-MEM_CHECK types are written with their type byte XOR'd against `_inputKey[0]` (the first byte of the session's RC4 input key). MEM_CHECK entries are written separately without an explicit type byte — only a module name index (usually 0), a 4-byte address, and a 1-byte length. The packet terminates with the raw `xorByte` value, allowing the client to recover the original check type by XOR.

The **response format** (inner opcode 0x02) contains: `[uint16 length][uint32 checksum]` followed by per-check results in request order. TIMING_CHECK returns `[uint8 result][uint32 clientTicks]`. MEM_CHECK returns `[uint8 result]` where 0 = match; non-zero is followed by the actual bytes read. PAGE_CHECK returns `[uint8 result]` where 0 = hash match. MODULE_CHECK and DRIVER_CHECK return a found/not-found byte. LUA_STR_CHECK returns `[uint8 strLen][string result]`. The checksum is validated server-side via `IsValidCheckSum()` before parsing individual results.

The `WARDEN_SMSG_MODULE_INITIALIZE` (opcode 0x03) packet provides function pointer offsets relative to `0x00400000` (WoW.exe base) for the module to call:

- **SFileOpenFile**: `0x006485F0`
- **SFileGetFileSize**: `0x006487F0`
- **SFileReadFile**: `0x00648460`
- **SFileCloseFile**: `0x00648730`
- **FrameScript::GetText**: `0x00819D40`
- **PerformanceCounter**: `0x0046AE20` (varies by emulator)

---

## Runtime memory: single RWX allocation, ~30KB, MEM_PRIVATE

The WoW client allocates memory for the Warden module using **`VirtualAlloc` with `MEM_COMMIT` and `PAGE_EXECUTE_READWRITE` (0x40)**. This produces a single contiguous allocation visible as a `MEM_PRIVATE` region — not mapped to any file on disk, which makes it detectable only through memory scanning.

The allocation size equals the `dwModuleSize` field from header offset 0x00. For the well-documented module in the namreeb/WardenSigning archive, the decompressed size is **30,469 bytes**; the final runtime size after section expansion is slightly larger due to the copy/skip unpacking expanding gaps. The entire module — header, code, data, resolved imports, and relocation-patched pointers — resides in this single allocation.

The **Warden module loader is embedded in WoW.exe** (32-bit), not a separate DLL. It can be located by hooking `VirtualAlloc` and filtering allocations > 8KB originating from Wow.exe's code section. The Warden scan procedure within the loaded module has a stable byte signature `\x56\x57\xFC\x8B\x54\x24\x14\x8B\x74\x24\x10\x8B\x44\x24\x0C\x8B\xCA\x8B\xF8\xC1\xE9\x02\x74\x02\xF3\xA5` — a memcpy-like function that copies target memory for hash comparison. This signature is notably **not polymorphically varied** across modules, unlike encryption functions.

Multiple RWX allocations are observed during module lifecycle — more than a dozen VirtualAlloc/VirtualFree cycles after login — reflecting the module's internal memory management for check buffers and temporary data. The client-side `WardenClient_HandlePacket` function is located at **`0x006CA5C0`** in the 3.3.5a binary.

---

## The three queried module hashes have no public documentation

The specific hashes **7C4ABC97**, **9A95D199**, and **DA3BF29E** returned zero results across all searched sources: OwnedCore, SkullSecurity, GitHub, getMaNGOS, wowdev.wiki, and general web search. These appear to be either partial/truncated MD5 hashes (Warden module names are full 32-character MD5 hex strings like `79C0768D657977D697E10BAD956CCED1`) or internal identifiers from unpublished reverse engineering work. The **namreeb/WardenSigning** repository contains 72 sniffed Warden modules with RC4 keys (archived from Neo2003), and **vmangos/warden_modules** hosts binary `.bin` + `.key` files — but neither indexes modules by partial hashes matching the queried values.

Publicly documented module hashes include `79C0768D657977D697E10BAD956CCED1` (1.12.1), `C128B52AD08980F905A2FCD5FF7424D1`, and `54B8EC6A00878BBD8A0757ED144053AF`. The Umbra Warden Explorer tool can restore PE characteristics from the custom format for IDA analysis, and the xakepru/x14.08-coverstory-blizzard repository contains C/C++ source code for client-side Warden interception including VirtualAlloc hooking, scan function patching, and module extraction.

---

## Key source code repositories and file paths

All major server-side emulators maintain Warden implementations in nearly identical directory structures:

| Repository | Key Path | Contents |
|---|---|---|
| TrinityCore/TrinityCore (3.3.5 branch) | `src/server/game/Warden/` | Warden.h, Warden.cpp, WardenWin.cpp, WardenCheckMgr.h/cpp |
| azerothcore/azerothcore-wotlk | `src/server/game/Warden/` | Same structure; adds `LUA_EVAL_CHECK` rename, pool categories |
| mangostwo/server | `src/game/Warden/` | Warden.h, Warden.cpp, WardenWin.cpp, WardenMac.cpp |
| mangoszero/server | `src/game/Warden/` | Includes `WardenState` enum with 7 states |
| cmangos/mangos-classic | `src/anticheat/Warden/` | WardenCheckMgr.h with check/result structs |
| namreeb/WardenSigning | `WardenSigning/`, `WardenModules/` | RSA verification code + 72 binary modules |
| vmangos/warden_modules | Root | Binary `.bin` + `.key` files for vanilla |
| xakepru/x14.08-coverstory-blizzard | `src/` | Client-side RE code (C/C++, HackMag article source) |
| tomrus88/WoWTools | `src/WoWPacketViewer/Parsers/Warden/` | C# packet parsers showing XOR key logic |

The Warden state machine progresses through: `STATE_INITIAL → STATE_REQUESTED_MODULE → STATE_SENT_MODULE → STATE_REQUESTED_HASH → STATE_INITIALIZE_MODULE → STATE_REQUESTED_DATA → STATE_RESTING`, cycling back to `STATE_REQUESTED_DATA` every **10–60 seconds** for periodic check requests.

---

## Conclusion

The Warden module system in WoW 3.3.5a represents a sophisticated code-shipping anti-cheat architecture. Three findings stand out. First, the custom binary format — while simpler than PE — achieves PE-equivalent functionality (relocations, named imports, section layout) in roughly 30KB with minimal overhead, making it efficient for network delivery. Second, the per-module variation in check type byte values is a deliberate obfuscation layer that the emulator community largely ignores by hardcoding values from a single module; anyone analyzing a different module (like those identified as 9A95D199 or DA3BF29E) will encounter completely different type constants and potentially different dispatch mechanisms. Third, the stable scan function signature across polymorphically varied modules represents an architectural weakness — one invariant code pattern in an otherwise deliberately varied binary — which the bypass community has exploited for reliable runtime interception. For the specific modules queried, direct IDA analysis of the decompressed, relocated binaries would be required, as no public documentation exists for their internal structures.