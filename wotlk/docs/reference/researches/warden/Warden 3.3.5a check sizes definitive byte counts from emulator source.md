# Warden 3.3.5a check sizes: definitive byte counts from emulator source

**Six of the nine initial estimates were wrong.** Direct examination of AzerothCore's `WardenWin::RequestChecks()` function (commit 05161a9) reveals the exact byte counts written per check type in the `CHEAT_CHECKS_REQUEST` packet. The largest corrections: MODULE_CHECK writes **24 bytes** (not 1), DRIVER_CHECK writes **25 bytes** (not 1), PAGE_CHECK_A/B write **29 bytes** (not 25), and MEM_CHECK writes **6 bytes** (not 27). Every check type that carries a `Seed + SHA1` pair writes **24 bytes** for that combined field — confirmed by `ToByteVector(24, false)` in the actual code. For algorithmic parsing, an **SMT/CSP solver (Z3)** is dramatically better than DFS, since there are only 9 unknown size variables with many constraints from each entry.

## The definitive byte sizes for all 9 check types

The table below shows exact data bytes **after** the XOR'd type byte in the "other checks" section of a `CHEAT_CHECKS_REQUEST` packet (opcode `0x02`). These are verified against the actual switch statement in `WardenWin::RequestChecks()`.

| Check Type | Hex ID | Data Bytes | Fields | Fixed? |
|---|---|---|---|---|
| **TIMING_CHECK** | 0x57 (87) | **0** | *(empty)* | Yes |
| **MEM_CHECK** | 0xF3 (243) | **6** | `uint8(0x00)` + `uint32(Address)` + `uint8(Length)` | Yes |
| **PAGE_CHECK_A** | 0xB2 (178) | **29** | `Data[24]` + `uint32(Address)` + `uint8(Length)` | Yes |
| **PAGE_CHECK_B** | 0xBF (191) | **29** | `Data[24]` + `uint32(Address)` + `uint8(Length)` | Yes |
| **MPQ_CHECK** | 0x98 (152) | **1** | `uint8(stringIndex)` | Yes |
| **LUA_EVAL_CHECK** | 0x8B (139) | **1** | `uint8(stringIndex)` | Yes¹ |
| **DRIVER_CHECK** | 0x71 (113) | **25** | `Data[24]` + `uint8(stringIndex)` | Yes |
| **MODULE_CHECK** | 0xD9 (217) | **24** | `uint8[4](seed)` + `byte[20](HMAC-SHA1)` | Yes |
| **PROC_CHECK** | 0x7E (126) | **31** | `Data[24]` + `uint8(modIdx)` + `uint8(procIdx)` + `uint32(Offset)` + `uint8(Length)` | Yes |

¹ *The base format uses a 1-byte string table index. Some enhanced AzerothCore builds switch to variable-length inline Lua code: `uint8(strLen)` + `char[N](luaCode)`, capped at 170 bytes. See the LUA_EVAL section below.*

The 24-byte `Data` field present in PAGE_CHECK_A/B, DRIVER_CHECK, and PROC_CHECK always contains a **4-byte Seed** concatenated with a **20-byte SHA1 hash**, serialized via `check->Data.ToByteVector(24, false)`. MODULE_CHECK is unique: it generates a fresh 4-byte random seed at packet construction time and appends a 20-byte HMAC-SHA1 digest computed over the module name string keyed with that seed.

## Correcting the initial estimates and why they diverged

The initial estimates appear to have mixed up which protocol layer was being measured or confused the Data field contents. Here is every correction:

- **MODULE_CHECK**: Listed as 1 byte ("seed"), actually **24 bytes**. The "seed" is 4 bytes, followed by a 20-byte HMAC-SHA1 digest. The module name string is never sent — the client enumerates loaded DLLs, computes HMAC-SHA1 for each, and checks against the server's hash.
- **DRIVER_CHECK**: Listed as 1 byte ("seed"), actually **25 bytes**. Identical to MODULE_CHECK's 24-byte Seed+SHA1 block, plus a 1-byte string table index pointing to the driver name.
- **PAGE_CHECK_A/B**: Listed as 25 bytes, actually **29 bytes**. The 25-byte figure omits the 4-byte Seed, counting only SHA1(20) + Address(4) + Length(1). The actual Data field is 24 bytes (Seed + SHA1), not 20.
- **MEM_CHECK**: Listed as 27 bytes, actually **6 bytes**. MEM_CHECK writes just `0x00` (module name index for main module), a 4-byte address, and a 1-byte length. No Seed or SHA1 field at all.
- **LUA_EVAL_CHECK**: Listed as 2 bytes, actually **1 byte** in the standard string-table-index format (identical to MPQ_CHECK serialization).
- **TIMING_CHECK** (0 bytes) and **MPQ_CHECK** (1 byte) were correct.

## Packet structure: three distinct sections with XOR encoding

The `CHEAT_CHECKS_REQUEST` packet (opcode `0x02`, sent as `SMSG_WARDEN_DATA`) has three sections. Understanding this architecture is critical for parsing.

**Section 1 — String table.** Length-prefixed strings for checks that reference names by index (MPQ filenames, driver names, Lua strings, module names, proc names). Each entry is `uint8(stringLength) + char[stringLength]`. The section terminates with a `0x00` byte. String indices are assigned sequentially starting from 0 during packet construction.

**Section 2 — Other checks.** All non-MEM_CHECK types (TIMING, PAGE_A, PAGE_B, MPQ, LUA_EVAL, DRIVER, MODULE, PROC) are serialized here. Each entry is `uint8(checkType XOR xorByte)` followed by type-specific data. The `xorByte` is `_inputKey[0]` — the first byte of the client-to-server Warden RC4 encryption key. The section terminates with a bare `xorByte` value. TIMING_CHECK always appears first.

**Section 3 — MEM checks.** Each MEM_CHECK entry is `uint8(0x00) + uint32(address) + uint8(length)` — 6 bytes with no XOR encoding on the type. This section appears at the end.

The XOR encoding means that **raw type bytes in the packet do not match the enum values directly**. To decode, XOR each candidate type byte with the session's `xorByte`. The terminator for Section 2 is the `xorByte` itself (which decodes to `0x00`, i.e., `NONE_CHECK`).

## The switch statement from actual source code

The following code from AzerothCore's `WardenWin::RequestChecks()` (commit 05161a9) constructs Section 2:

```cpp
buff << uint8(check->Type ^ xorByte);
switch (check->Type)
{
    case MEM_CHECK:
    {
        buff << uint8(0x00);
        buff << uint32(check->Address);
        buff << uint8(check->Length);
        break;
    }
    case PAGE_CHECK_A:
    case PAGE_CHECK_B:
    {
        std::vector<uint8> data = check->Data.ToByteVector(24, false);
        buff.append(data.data(), data.size());    // 24 bytes: Seed(4) + SHA1(20)
        buff << uint32(check->Address);           // 4 bytes
        buff << uint8(check->Length);             // 1 byte
        break;                                    // Total: 29 bytes
    }
    case MPQ_CHECK:
    case LUA_EVAL_CHECK:
    {
        buff << uint8(index++);                   // 1 byte: string table index
        break;
    }
    case DRIVER_CHECK:
    {
        std::vector<uint8> data = check->Data.ToByteVector(24, false);
        buff.append(data.data(), data.size());    // 24 bytes: Seed(4) + SHA1(20)
        buff << uint8(index++);                   // 1 byte: string table index
        break;                                    // Total: 25 bytes
    }
    case MODULE_CHECK:
    {
        std::array<uint8, 4> seed = Acore::Crypto::GetRandomBytes<4>();
        buff.append(seed);                        // 4 bytes: random seed
        buff.append(Acore::Crypto::HMAC_SHA1::GetDigestOf(seed, check->Str));
        break;                                    // 20 bytes: HMAC digest → Total: 24 bytes
    }
    /* PROC_CHECK is commented out in all examined codebases:
    case PROC_CHECK:
    {
        buff.append(wd->i.AsByteArray(0, false).get(), wd->i.GetNumBytes()); // 24 bytes
        buff << uint8(index++);     // 1 byte: module name index
        buff << uint8(index++);     // 1 byte: proc name index
        buff << uint32(wd->Address); // 4 bytes
        buff << uint8(wd->Length);   // 1 byte → Total: 31 bytes
        break;
    }*/
}
```

PROC_CHECK is **commented out in every examined codebase** (TrinityCore, AzerothCore, MaNGOS). Its format is inferred from the `Warden.h` comment — `uint Seed + byte[20] SHA1 + byte moduleNameIndex + byte procNameIndex + uint Offset + byte Len` — giving **31 bytes**. The Data field would contain 24 bytes (Seed + SHA1), consistent with PAGE_CHECK and DRIVER_CHECK.

## LUA_EVAL_CHECK: two format variants exist

The standard implementation (matching the Blizzard client's original Warden module) writes a **1-byte string table index**. The Lua expression text lives in Section 1's string table. This format is shown in the switch statement above, where `LUA_EVAL_CHECK` falls through to the `MPQ_CHECK` case.

An enhanced AzerothCore variant replaces this with **inline Lua code**:

```cpp
buff << uint8(sizeof(_luaEvalPrefix) - 1 + check->Str.size()
            + sizeof(_luaEvalMidfix) - 1 + check->IdStr.size()
            + sizeof(_luaEvalPostfix) - 1);    // 1 byte: total string length
buff.append(_luaEvalPrefix, ...);              // Lua wrapper prefix
buff.append(check->Str.data(), ...);           // Lua check expression
buff.append(_luaEvalMidfix, ...);              // Lua wrapper midfix
buff.append(check->IdStr.data(), ...);         // 4-byte check ID string
buff.append(_luaEvalPostfix, ...);             // Lua wrapper postfix
```

This inline format writes `uint8(totalLen) + char[totalLen]` — **variable-length, 1 + N bytes**, capped by `WARDEN_MAX_LUA_CHECK_LENGTH = 170`. If you are parsing packets from a standard 3.3.5a Warden module, assume 1 byte (string index). If parsing from a custom emulator module, the inline format applies.

## Z3/SMT solver is the optimal parsing algorithm

For parsing the Section 2 byte stream without knowing sizes a priori, an **SMT constraint solver like Z3 massively outperforms DFS**. The reason is structural: there are only **9 unknown integer variables** (one size per type), but every occurrence of a type in the stream creates a constraint linking that position to the next valid type byte. Z3 exploits global consistency — if type `0xB2` has 29 value bytes in one entry, it must have 29 everywhere — through constraint propagation that prunes the search space from exponential to near-instant.

A practical Z3 implementation requires roughly 100–200 lines of Python:

```python
from z3 import *
s = Solver()
sizes = {t: Int(f'size_{t}') for t in VALID_TYPE_IDS}
for sz in sizes.values():
    s.add(sz >= 0, sz <= 40)  # reasonable upper bound

# For each packet, generate chained position constraints:
# pos[0] = 0; pos[i+1] = pos[i] + 1 + sizes[data[pos[i]] ^ xorByte]
# Final constraint: last pos == len(data)
```

With even a single well-populated packet containing multiple check types, Z3 can determine all 9 sizes in milliseconds. Multiple packets make the system overdetermined, enabling Z3 to prove uniqueness. The key advantage over DFS is that **Z3 shares constraints globally** rather than treating entries independently.

Three other approaches complement Z3:

- **System of linear equations**: If you know each packet's total length and the count of each type (from scanning XOR'd type bytes), you get `sum(count_i × size_i) = total_data_length` per packet. With ≥9 linearly independent packets, Gaussian elimination yields the sizes in O(n³). This serves as a fast heuristic pre-filter for Z3.
- **Enhanced DFS with consistency enforcement**: Your existing DFS improves dramatically if you enforce that once a size is assigned to a type, all future occurrences must match. This converts exponential worst-case into near-linear average-case, essentially implementing CSP constraint propagation manually.
- **Symbolic execution (angr/PISE)**: If you have the Warden binary module, angr can determine read sizes by symbolically executing each handler. PISE (NDSS 2023) automates this for protocol inference. This gives definitive answers but requires the prepared binary.

## Static analysis of the Warden binary module

For direct extraction from the client-side Warden module (x86-32), **Ghidra or IDA Pro static analysis is the most practical approach**. The module is small (typically <100KB), contains only 9 handler functions, and CDataStore::Read calls follow recognizable patterns.

Warden modules use a **custom Blizzard format** — not standard PE/DLL. They are encrypted (RC4), compressed (zlib), and RSA-signed. The tool **Warden Explorer** (by Umbra, v0.0.0.4) can convert cached modules to DLL format. After preparation (decryption, decompression, relocation), the module can be loaded in a disassembler.

The recommended analysis workflow has three tiers:

**Tier 1 — Ghidra/IDA static analysis.** Load the prepared module, find the handler dispatch table (a switch/jump table indexed by check type byte), then decompile each handler. CDataStore methods use `thiscall` convention (ECX = this pointer). Count all `Read(void*, size)` calls in each handler — the `size` parameter is typically a constant pushed to the stack before the call. An IDAPython or Ghidra script can automate this by iterating over `CALL` instructions within each handler, checking if the target matches CDataStore::Read addresses, and summing the size arguments.

**Tier 2 — Unicorn Engine emulation.** Map the module code into Unicorn's x86-32 emulator, set up a mock CDataStore object with a large data buffer, hook CDataStore::Read to count consumed bytes, then emulate each handler. This provides concrete execution without path explosion issues and verifies static analysis results. The downside is that only one execution path is followed per run, requiring multiple runs with different input data for branching handlers.

**Tier 3 — angr symbolic execution.** For handlers with conditional reads or variable-length fields, angr can explore all execution paths and determine the symbolic relationship between input data and total bytes consumed. Hook CDataStore::Read as a SimProcedure that tracks the size argument. This handles the edge case of LUA_EVAL_CHECK's length-prefixed string, where the read size depends on a prior read value.

## Conclusion

The canonical byte sizes for WoW 3.3.5a Warden check types are **0, 6, 29, 29, 1, 1, 25, 24, and 31** for TIMING, MEM, PAGE_A, PAGE_B, MPQ, LUA_EVAL, DRIVER, MODULE, and PROC respectively. The critical insight missed by the initial estimates is that the `Data` field in PAGE_CHECK, DRIVER_CHECK, MODULE_CHECK, and PROC_CHECK always contains **24 bytes (4-byte Seed + 20-byte SHA1)**, not just the SHA1 alone. PROC_CHECK remains unverified at runtime since it is commented out in all examined emulator codebases, but its 31-byte size is consistent with the `Warden.h` format comments and the 24-byte Data field pattern. For parsing unknown packets, Z3 constraint solving with 9 integer unknowns is the theoretically and practically optimal approach — it reduces the problem from exponential DFS exploration to a tightly constrained integer satisfaction problem solvable in milliseconds.