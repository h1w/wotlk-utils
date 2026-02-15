# Warden module dispatch internals and type ID extraction

**Warden modules use module-specific check type byte values dispatched through a jump table inside a monolithic "Warden::Process" function, and the xorByte at `[reg+4]` is the same value as the last byte of the CHEAT_CHECKS_REQUEST packet — derived server-side from `_inputKey[0]`.** Every open-source emulator hardcodes these type IDs for a single canonical module (hash `79C0768D657977D697E10BAD956CCED1`), meaning your scanner must extract them per-module from the binary. The two dispatch patterns you identified (cmp+jcc chain and 256-entry remap table) are the known variants; no additional mechanisms have been publicly documented, but Blizzard intentionally compiled structurally different module variants to frustrate automated analysis.

## The dispatch lives inside a single enormous function

Reverse engineering of the 4.3.4 (15595) Warden module by namreeb revealed that the core dispatch is a function he termed **`Warden::Process`** — described as "huge" because "it contains a jump table and handles most scans." This function is called by the module's `PacketHandler` export (offset `+0x08` in the `WardenFuncList` vtable) when a `WARDEN_SMSG_CHEAT_CHECKS_REQUEST` (opcode 0x02) arrives. It receives the module context via ECX (`__thiscall`), a SHA1 digest via ESI, and file-system function pointers via EDX.

The internal flow is: read the **xorByte from the last byte of the packet**, store it in the context structure, then loop through the packet stream reading each encoded type byte, XOR-decoding it (`actualType = rawByte ^ xorByte`), and dispatching to the corresponding handler. Your two observed patterns — the **cmp+jcc sequential comparison chain** and the **256-entry remap table + indirect jump table** — are both compiled representations of this same logical switch statement. The cmp+jcc chain is a straightforward if-else cascade the compiler emits for sparse case values. The remap table is an optimization where the compiler generates a 256-byte lookup that maps the decoded type byte to a dense handler index (0–N), then uses that index into a function-pointer or offset jump table. Both encode the same information: a mapping from type byte values to handler entry points.

**No other dispatch mechanisms** (computed goto, hash-based dispatch, hybrid approaches) have been documented in the public reverse engineering community. However, Blizzard deliberately compiled multiple structurally distinct module variants. As one community researcher noted, modules are "written/compiled in several forms so that reverse engineering is very difficult." This means your scanner's failure on module `3E02C87EB2B3D5D29C5E9626E2B59AE6` likely stems from a variant that uses a dispatch pattern subtly different from the two you've codified — possibly a compiler-generated binary search tree for the sparse case values, or an inlined sequence with different register allocation that breaks your pattern match. That specific module hash does not appear in namreeb's 72-module archive, the vmangos/warden_modules repository, or any public forum discussion.

## Open-source emulators all hardcode one module's type IDs

Every major emulator — **TrinityCore, AzerothCore, MaNGOS Zero/Two, CMaNGOS, OregonCore** — uses identical hardcoded enum values for check types, all derived from reverse engineering the canonical module `79C0768D657977D697E10BAD956CCED1`:

| Check category | Hex value | Decimal | Request data size |
|---|---|---|---|
| TIMING_CHECK | 0x57 | 87 | 0 bytes |
| DRIVER_CHECK | 0x71 | 113 | 25 bytes |
| PROC_CHECK | 0x7E | 126 | 31 bytes |
| LUA_STR_CHECK | 0x8B | 139 | 1 byte |
| MPQ_CHECK | 0x98 | 152 | 1 byte |
| PAGE_CHECK_A | 0xB2 | 178 | 29 bytes |
| PAGE_CHECK_B | 0xBF | 191 | 29 bytes |
| MODULE_CHECK | 0xD9 | 217 | 24 bytes |
| MEM_CHECK | 0xF3 | 243 | 6 bytes |

**These values are NOT universal.** As explicitly confirmed by community researchers: "The CheckTypes change depending on the warden module you have loaded into the game." The emulators hardcode them because they ship only the one canonical module. No emulator project extracts type IDs dynamically from the module binary. ArcEmu does not implement the full Warden check system at all — it relies on server-side movement validation instead.

The server constructs the CHEAT_CHECKS_REQUEST packet with `xorByte = _inputKey[0]` (first byte of the C→S Warden RC4 key, derived from the session key K via `SHA1Randx`). Each type byte is written as `type ^ xorByte`. The xorByte itself is appended as the final byte of the packet. The packet structure is: opcode byte → string table (length-prefixed strings for MEM_CHECK module names, LUA strings, MPQ filenames, driver names, terminated by 0x00) → per-check type+data section (each check's XOR'd type byte followed by its payload) → raw xorByte terminator. The entire buffer is then RC4-encrypted.

## The xorByte context field at [reg+4] explained

The module's context structure is rooted at the `WardenFuncList**` pointer passed as the `this` parameter (ECX) to all exported functions. Based on SkullSecurity wiki documentation and cross-referencing with the emulator code:

| Offset | Field | Description |
|---|---|---|
| +0x00 | `ppFuncList` | Pointer to the exported function table (GenerateRC4Keys, Unload, PacketHandler, Tick) |
| +0x04 | `xorByte` | Stored XOR key for decoding check type bytes |
| +0x08+ | Various | Module base pointer, RC4 crypto contexts, state buffers |

**Yes, `[reg+4]` is the xorByte, and it holds the same value as the last byte of the CHEAT_CHECKS_REQUEST packet.** The flow is: the module's PacketHandler reads the last byte of the decrypted packet, stores it at context+4, then uses it in the dispatch loop to XOR-decode each type byte encountered while iterating through the check stream. It is not a persistent field across packets — it is overwritten each time a new CHEAT_CHECKS_REQUEST arrives.

The Cataclysm-era module analysis revealed function pointer offsets within the context/vtable structure for individual handlers: `Warden_MemoryCheck = 0xD0`, `Warden_ModuleCheck = 0xD4`, `Warden_DriverCheck = 0xE4`, `Warden_TimingCheck = 0xE8`, `Warden_StorePageScanInfo = 0xF4`. These offsets are module-version-specific but demonstrate that the handler dispatch table is embedded in the module's context structure at fixed offsets from the base.

## Reliable techniques for extracting type-to-category mappings

The most robust extraction strategy uses **handler identification by data consumption pattern**, working backward from handlers to type IDs through the dispatch table. Here is the recommended approach:

**Step 1: Locate the PacketHandler export.** The module's initialization function returns a `WardenFuncList**`. The PacketHandler is at offset +0x08 in that function table. The init function itself is found via the export table: read `dwExportTableOffset` (header offset 0x10), compute `EDX = 1 - dwBaseIndex` (header offset 0x18), then resolve the function pointer at `exportTable + EDX*4`.

**Step 2: Find the dispatch function.** Inside the PacketHandler, look for the call to the inner `Warden::Process` function — it is the largest function in the module and contains the switch/jump table. The signature to look for is: a loop that reads bytes from a buffer, XORs with a value loaded from the context structure (offset +4), and then either compares against constants (cmp+jcc) or indexes into a table.

**Step 3: Extract the type-to-handler mapping.** For the remap-table variant: locate the 256-byte table (referenced via `movzx byte [reg+large_displacement]`). Each entry maps a decoded type byte to a handler index. Read the full table to get the complete mapping. For the cmp+jcc variant: extract the comparison immediate values from each `cmp al, imm8` instruction — these are the type IDs directly.

**Step 4: Identify handlers by data consumption.** Each handler reads a deterministic number of bytes from the packet stream. Trace each handler's reads to determine its consumption size, then match against the known sizes:

- **0 bytes** → TIMING_CHECK (handler does no stream reads; it simply records a timestamp)
- **1 byte** → MPQ_CHECK or LUA_STR_CHECK (both read a single string-table index; distinguish by whether the handler hashes an MPQ file or evaluates a Lua string)
- **6 bytes** (1+4+1) → MEM_CHECK (reads module name index, uint32 offset, byte length)
- **24 bytes** (4+20) → MODULE_CHECK (reads uint32 seed + 20-byte SHA1)
- **25 bytes** (4+20+1) → DRIVER_CHECK (reads seed + SHA1 + driver name index)
- **29 bytes** (4+20+4+1) → PAGE_CHECK_A or PAGE_CHECK_B (reads seed + SHA1 + address + length; distinguish by whether the handler checks all pages or only MZ+PE pages — PAGE_CHECK_B calls an additional MZ/PE header validation)
- **31 bytes** (4+20+1+1+4+1) → PROC_CHECK (reads seed + SHA1 + two string indices + offset + length)

**Step 5: Disambiguate same-size handlers.** MPQ_CHECK vs LUA_STR_CHECK (both 1 byte): look for calls to SFileOpenFile/SFileReadFile/SFileGetFileSize (MPQ) vs FrameScript::GetText (Lua). PAGE_CHECK_A vs PAGE_CHECK_B: both read 29 bytes, but PAGE_CHECK_B includes a MZ+PE header signature check (comparing against `0x5A4D` and reading the PE offset).

**Alternative approach — Mac module cross-reference.** Blizzard shipped Mach-O format Warden modules for Mac clients that retain symbol names. Cross-referencing the Mac module's named handler functions with the corresponding Windows module (same logical version) provides ground-truth handler identification without needing to reverse each handler's internal logic.

## Module binary format reference for your loader

The decompressed module starts with a **40-byte header** with these confirmed fields from the SkullSecurity wiki:

| Offset | Size | Field |
|---|---|---|
| 0x00 | 4 | `dwModuleSize` — total size of loaded module in memory |
| 0x04 | 4 | (reserved/unknown) |
| 0x08 | 4 | `dwRelocOffset` — offset to relocation data |
| 0x0C | 4 | `dwRelocCount` — number of relocation entries |
| 0x10 | 4 | `dwExportTableOffset` — offset to export function pointer table |
| 0x14 | 4 | `dwExportCount` — number of exported functions |
| 0x18 | 4 | `dwBaseIndex` — base index for export resolution (init = export[1 - baseIndex]) |
| 0x1C | 4 | `dwImportTableOffset` — offset to import library table |
| 0x20 | 4 | `dwImportCount` — number of imported libraries |
| 0x24 | 4 | `dwSectionCount` — number of sections |

Section data starts at offset `0x28 + (dwSectionCount * 12)`. The **RLE packing** uses alternating copy/skip runs: read a 2-byte little-endian count, copy that many bytes from source (or skip/zero-fill if it's a skip run), toggle the copy/skip flag, repeat until `dwModuleSize` bytes are filled. First run is always a copy. **Delta-encoded relocations** use 2-byte big-endian deltas (high bit clear = delta from previous address) or 4-byte absolute offsets (high bit set, rare). Each relocation target DWORD is adjusted by adding the load base address.

The import table has `dwImportCount` entries of 8 bytes each: 4-byte offset to library name string + 4-byte offset to IAT. The IAT is a null-terminated DWORD array where positive values are offsets to function name strings and negative values (high bit set) are ordinal imports. After resolution, IAT entries are overwritten in-place with resolved function addresses.

## Conclusion

Your scanner's misidentification on module `3E02C87EB2B3D5D29C5E9626E2B59AE6` most likely results from a third dispatch pattern — probably a **compiler-generated binary search tree** over the sparse type values, which would look like nested cmp+jcc sequences but with a different branching structure than a linear comparison chain. To make your scanner robust across all modules, the most reliable approach is to avoid pattern-matching the dispatch mechanism entirely. Instead, locate the PacketHandler export via the header's export table, find the inner dispatch function by looking for the xor-with-context-field pattern, then extract the mapping from whatever dispatch structure is present — whether it's a remap table (look for 256-byte data references), a jump table (look for `jmp [reg*4+base]`), or a comparison chain (extract all `cmp` immediates reachable from the XOR instruction). Handler identification by data consumption size is the most portable fingerprinting technique and requires no knowledge of the specific dispatch encoding used.