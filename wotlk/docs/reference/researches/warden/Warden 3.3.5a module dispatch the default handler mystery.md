# Warden 3.3.5a module dispatch: the default handler mystery

**The default handler at index 0x0A almost certainly consumes zero additional request bytes and writes zero response bytes — acting as a pure no-op skip.** This design allows the module to silently ignore unrecognized check type bytes without breaking the sequential parsing of the request stream, but only when the unknown type has no trailing parameter data. The critical finding from your reverse engineering is that the TrinityCore/AzerothCore hardcoded enum values (including TIMING_CHECK = 0x57) are likely mismatched against the specific Warden module you're analyzing, meaning the server believes 0x57 is TIMING_CHECK while your module routes it to the fallback. This explains the "valid but unexpected" response behavior you observed.

## The 256-byte dispatch table and XOR decode pipeline

The Warden module's check type dispatch works through a two-stage decode. First, the module reads the **xorByte from the final byte** of the decrypted CHEAT_CHECKS_REQUEST packet. The server constructs this byte as `_inputKey[0]` — the first byte of the session's RC4 input key. Every check type byte on the wire is XOR'd with this value: `wire_byte = canonical_type ^ xorByte`. The module reverses this by XOR'ing each wire byte back, recovering the canonical type value.

The 256-byte table you found is then a **static canonical-value-indexed lookup** compiled into the module binary. Position `table[canonical_type]` yields the handler index (0x00–0x0A). Since the XOR key changes every session but the table is static, the table must operate on decoded canonical values, not raw wire bytes. Your finding of **10 valid handler entries and 216 default entries** among 226 reachable positions is consistent with 9 check categories plus one variant (your "extra PAGE"), with the remaining byte positions falling through to 0x0A.

The binary search tree (CMP/JE/JNE chain) you identified is likely the **compiler-generated switch dispatch** that the module uses after the table lookup, routing each handler index to its corresponding function. Some modules may use the table as a fast O(1) index, others may compile the dispatch as a balanced comparison tree — both patterns are common in optimized x86 code generated from switch statements with sparse case values.

## What handler 0x0A actually does

No public reverse engineering source explicitly documents the default handler's byte-level behavior. However, architectural constraints strongly indicate it is a **zero-read, zero-write no-op**:

The check request format uses **variable-length entries** — MEM_CHECK has 6 parameter bytes, PAGE_CHECK_A/B has 29 bytes, MODULE_CHECK has 24 bytes, while TIMING_CHECK has **zero additional bytes**. A default handler cannot safely skip an unknown check type's parameter data because it has no way to know how many bytes to consume. Reading the wrong number of bytes would desynchronize the parser and corrupt all subsequent check entries. The only universally safe behavior is to consume nothing beyond the type byte (which the dispatch logic already consumed) and produce nothing in the response. This works correctly when the unknown type happens to carry zero parameter bytes — exactly like TIMING_CHECK's wire format.

Your observation of **11 bytes of result data with correct SHA1 checksum** corroborates this. The CMSG_WARDEN_DATA response for opcode 0x02 has this structure:

- **2 bytes** — `uint16` Length (size of result data)
- **4 bytes** — `uint32` Checksum (SHA1 XOR-folded to 32 bits)
- **N bytes** — Result data (covered by Checksum)

If the only check producing output was a properly-handled TIMING_CHECK (via whatever canonical byte your module recognizes), the result data would be **5 bytes**: 1 byte result flag + 4 bytes `GetTickCount()` value. Total packet body after the opcode byte: **2 + 4 + 5 = 11 bytes**. The default handler for 0x57 contributed zero bytes to this total, which is exactly what a no-op skip would produce. The checksum is computed as `SHA1(result_data)` folded via XOR of five 32-bit words into a single `uint32`.

## The TrinityCore/AzerothCore enum mismatch problem

The server emulators hardcode check type byte values directly in an enum and **do not read the module's internal dispatch table**. The relevant enum from `Warden.h`:

```cpp
enum WardenCheckType
{
    MEM_CHECK       = 0xF3,
    PAGE_CHECK_A    = 0xB2,
    PAGE_CHECK_B    = 0xBF,
    MPQ_CHECK       = 0x98,
    LUA_EVAL_CHECK  = 0x8B,
    DRIVER_CHECK    = 0x71,
    TIMING_CHECK    = 0x57,
    PROC_CHECK      = 0x7E,
    MODULE_CHECK    = 0xD9,
};
```

These values were reverse-engineered from **one specific Warden module version**. The `RequestChecks()` function in `WardenWin.cpp` writes them directly: `buff << uint8(check->Type ^ xorByte)`. There is no dynamic discovery of what byte values the loaded module actually expects. The server has **no mechanism to read the module's 256-byte remap table** or verify that its enum values match the module's dispatch entries.

This means a **mismatch is not just possible — it is the most likely explanation** for your 0x57 observation. If your module was compiled with a different set of canonical type assignments than TrinityCore's enum, then sending 0x57 as TIMING_CHECK would indeed route to handler 0x0A (default) inside the module. The namreeb/WardenSigning repository documents **72 different sniffed Warden modules** with different MD5 hashes, and while no public analysis confirms that different modules use different canonical type values, the architecture clearly supports it. Blizzard could rotate type byte assignments across module versions as an anti-RE measure.

The `RequestChecks()` function in AzerothCore builds the packet in two phases. Phase 1 writes the string table (module names, Lua eval strings, MPQ file paths). Phase 2 writes check type bytes XOR'd with `xorByte` and their parameters. TIMING_CHECK is always injected first with zero parameter bytes, followed by a configurable mix of MEM, LUA, and "other" checks selected from rotating pools (`_ChecksTodo[WARDEN_CHECK_MEM_TYPE]`, `_ChecksTodo[WARDEN_CHECK_LUA_TYPE]`, `_ChecksTodo[WARDEN_CHECK_OTHER_TYPE]`). The packet terminates with the raw `xorByte` as a sentinel, then the entire buffer is RC4-encrypted.

## More than 9 check categories is plausible

TrinityCore defines exactly **9 check types** in its enum. Your finding of 10 handler indices (0x00–0x09) handling 9 named categories plus an "extra PAGE" variant is consistent with Blizzard's module having **a split handler** — likely PAGE_CHECK_A and PAGE_CHECK_B sharing behavioral logic but having distinct handler index entries, plus a third PAGE variant that emulators never implemented. Alternatively, PROC_CHECK (which is **commented out** in AzerothCore's `RequestChecks()` and marked as non-functional) might have been active in Blizzard's original module with a distinct handler.

The server-side code groups checks into just three scheduling pools — `WARDEN_CHECK_MEM_TYPE` (MEM_CHECK), `WARDEN_CHECK_LUA_TYPE` (LUA_EVAL_CHECK), and `WARDEN_CHECK_OTHER_TYPE` (everything else) — so the individual handler count inside the module is invisible to the emulator's scheduling logic. The emulator would never know if a module had 9, 10, or 15 internal handlers; it only cares about the wire protocol.

Custom private-server checks (beyond Blizzard's original 9 types) cannot be injected into the module's dispatch table because the module binary is **RSA-signed** with a 2048-bit key (`e=0x10001`, 256-byte modulus). The signature validates `SHA1(module_data + "MAIEV.MOD")`. Without Blizzard's private key, a custom server cannot modify the module to add new handler entries. Any type byte not in the module's original dispatch table will unconditionally fall through to 0x0A.

## Response byte accounting per check type

The server's `HandleCheckResult()` function parses the response assuming this per-check byte layout:

| Check type | Response bytes | Details |
|---|---|---|
| TIMING_CHECK | **5** | 1 result byte + 4 `uint32` ticks |
| MEM_CHECK | **1 + N** | 1 result byte (0x00 = match); if match, N bytes of memory (N = requested Length) |
| PAGE_CHECK_A/B | **1** | 0xE9 = hash matched (pass) |
| MODULE_CHECK | **1** | 0xE9 = not found (pass) |
| DRIVER_CHECK | **1** | 0xE9 = not found (pass) |
| MPQ_CHECK | **20** | SHA1 hash of file contents |
| LUA_EVAL_CHECK | **1 + N** | 1 byte string length; if >0, N bytes of Lua result string |
| PROC_CHECK | **1** | 0xE9 = pass |

If a check type goes through handler 0x0A (default no-op), it contributes **zero bytes** to the response. This creates a **parsing desynchronization** on the server side if the server expects result bytes for that check. When the server tries to read the expected result bytes, it reads into the next check's data, cascading errors through the entire response parse. The server's checksum validation (SHA1 XOR-fold of the result data) may still pass if the data is structurally intact — the checksum validates data integrity, not semantic correctness.

For your specific case: the server sent 0x57 (believing it's TIMING_CHECK), the module's default handler produced 0 response bytes, and the server then attempted to read 5 bytes (1 timing result + 4 ticks) from whatever followed. If 0x57 was the only "other" check and TIMING was handled correctly via its actual canonical byte, the 11-byte response (2 Length + 4 Checksum + 5 timing data) would be consistent with a single properly-handled TIMING_CHECK plus a zero-byte contribution from the defaulted 0x57.

## Conclusion

The Warden module dispatch architecture is more self-contained than TrinityCore's server code assumes. The 256-byte static dispatch table, the per-session XOR obfuscation, and the RSA-locked module binary create a system where **the server must know the correct canonical type bytes for the specific module it loads** — but open-source emulators hardcode a single set of values with no discovery mechanism. Your finding that 0x57 routes to the default handler strongly suggests a module-version mismatch: the module you reversed uses different canonical type assignments than TrinityCore's enum. The default handler's no-op behavior (zero bytes consumed, zero bytes produced) is architecturally mandated by the variable-length request format — any other behavior would corrupt sequential parsing. To definitively confirm, cross-reference the 10 non-default entries in your 256-byte table against the check handler function bodies: each handler's parameter-parsing code will reveal which canonical byte maps to which check category in your specific module version.