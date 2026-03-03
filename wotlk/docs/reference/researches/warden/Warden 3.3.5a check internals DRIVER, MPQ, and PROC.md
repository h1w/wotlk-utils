# Warden 3.3.5a check internals: DRIVER, MPQ, and PROC

The Warden anti-cheat in WoW build 12340 uses a dynamically loaded client module to execute server-requested integrity checks. **For DRIVER_CHECK, PROC_CHECK, MODULE_CHECK, and PAGE_CHECK, the "pass" response byte is `0xE9`** — confirmed by MaNGOS Two and MaNGOS Zero source code with an explicit `const uint8 byte = 0xE9` comparison. The client performs local HMAC-SHA1 comparison for seed-based checks and returns only a single result byte; the server never sees raw scan data for these check types. MPQ_CHECK follows a different pattern: status byte `0x00` plus 20 bytes of plain SHA1 for server-side comparison.

The three check types serve distinct detection purposes. DRIVER_CHECK detects loaded kernel drivers (VM or cheat-tool). MPQ_CHECK verifies game data file integrity. PROC_CHECK detects function-level API hooking in system DLLs. All three are defined in the `warden_checks` database table and selected in randomized rotation by the server.

---

## DRIVER_CHECK: device-open probe returning a single byte

### Client-side algorithm

The Warden module executes DRIVER_CHECK by constructing a device path (e.g., `\\.\vmmemctl`) and calling **`CreateFileW`** to attempt opening the driver's device object. Some Warden module versions supplement this with **`NtQuerySystemInformation(SystemModuleInformation)`** (class `0x0B`) to enumerate loaded kernel modules and match filenames. The primary method documented for 3.3.5a-era modules is the CreateFileW approach — it checks whether the device is accessible, not whether the driver binary exists on disk.

The Warden client module's internal function table places the driver check handler at **offset `0xE4`** from the module base (per OwnedCore reverse engineering of the WotLK .mod file).

### Request format (server → client)

| Field | Size | Source |
|-------|------|--------|
| Type byte | 1 byte | `0x71` (113) |
| Seed | 4 bytes (uint32) | First 4 bytes of `data` column |
| SHA1 | 20 bytes | Remaining 20 bytes of `data` column |
| driverNameIndex | 1 byte | Index into string table |

The **seed + SHA1 = `HMAC-SHA1(seed_as_key, driverName)`**. This is confirmed by the MODULE_CHECK code path in MaNGOS Zero, which generates these dynamically: `HmacHash hmac(4, (uint8*)&seed); hmac.UpdateData(wd->str); hmac.Finalize();`. For DRIVER_CHECK, the values are pre-computed and stored in the `data` column of `warden_checks` (24 bytes as binary or hex). The driver name is also sent via the string table, so the HMAC functions as **request authentication** — it proves the server legitimately owns the check definition. The client verifies the HMAC matches the driver name string before executing the check, preventing abuse of the Warden module for arbitrary system scanning by unauthorized servers.

### Response format and validation

The response is **exactly 1 byte**:

```cpp
case PAGE_CHECK_A:
case PAGE_CHECK_B:
case DRIVER_CHECK:
case MODULE_CHECK:
{
    const uint8 byte = 0xE9;
    if (memcmp(buff.contents() + buff.rpos(), &byte, sizeof(uint8)) != 0)
    {
        // FAIL — driver WAS found
        checkFailed = *itr;
    }
    buff.rpos(buff.rpos() + 1);
}
```

**`0xE9` = pass** (driver not found). **Any value ≠ `0xE9` = fail** (driver found). No additional data is returned on failure — the server already knows which driver was checked from the check ID. The exact fail byte value is not documented as a specific constant; it is simply "not 0xE9."

### Penalty policy

The action is determined per-check by the `WardenActions` enum: **`0` = log only, `1` = kick, `2` = ban**. Ban duration is configured via `Warden.BanDuration` (default **86400 seconds / 24 hours**, `0` = permanent). The action can be overridden per check ID using the `warden_action` table in the characters database. The server configuration key `Warden.ClientCheckFailAction` sets the global default.

### Drivers typically checked

Standard open-source databases include VM detection drivers: **`vmmemctl`** (VMware), **`vboxguest`** and **`VBoxSF`** (VirtualBox), **`vmci`** (VMware), **`prl_fs`** (Parallels). Custom server configurations add known cheat-tool kernel drivers. The checks are designed to detect the driver's presence — in all standard implementations, finding the driver means failure.

### Safety of always returning 0xE9

For standard TrinityCore/AzerothCore/MaNGOS-based servers, **always returning `0xE9` is safe**. The source code comment explicitly states DRIVER_CHECK is to "check to ensure driver isn't loaded" — the expected clean-system result is always 0xE9 (not found). No open-source implementation contains "reverse checks" expecting a driver to be present. However, a custom server could theoretically add such checks. The Warden protocol itself does not prevent this — the server controls interpretation — but no evidence of this practice exists in any analyzed codebase or community discussion.

---

## MPQ_CHECK: file hash verification through Storm.dll

### Client-side algorithm

The Warden module opens files from MPQ archives using Blizzard's internal **Storm/SFile API**. During module initialization (via `WARDEN_SMSG_MODULE_INITIALIZE`, opcode `0x03`), the server provides the Warden module with function addresses for the 3.3.5a client:

| Function | Offset (from 0x00400000) |
|----------|--------------------------|
| SFileOpenFile | `0x002485F0` |
| SFileGetFileSize | `0x002487F0` |
| SFileReadFile | `0x00248460` |
| SFileCloseFile | `0x00248730` |

The client calls `SFileOpenFileEx` to open the named file within the MPQ archive chain, `SFileReadFile` to read its entire contents, and computes a **plain SHA1** hash of the complete file content. There is no seed, no HMAC, no partial read — it hashes the full file.

### Request format (server → client)

| Field | Size | Source |
|-------|------|--------|
| Type byte | 1 byte | `0x98` (152) |
| fileNameIndex | 1 byte | Index into string table |

This is the simplest check format. **No seed or SHA1 fields are sent in the request.** The server relies entirely on comparing the returned hash against a stored expected value.

### Response format and validation

The response is **1 byte status + 20 bytes SHA1**:

```cpp
case MPQ_CHECK:
{
    uint8 result;
    buff >> result;
    if (result != 0x00)
    {
        // File could not be opened/read — FAIL
        checkFailed = *itr;
    }
    else
    {
        if (memcmp(buff.contents() + buff.rpos(),
                   rs->Result.AsByteArray(0, false), 20) != 0)
        {
            // SHA1 mismatch — FAIL (file was modified)
            checkFailed = *itr;
        }
        buff.rpos(buff.rpos() + 20);
    }
}
```

Status **`0x00`** = file successfully read (followed by 20-byte SHA1). **Non-zero status** = file could not be opened (treated as fail, no SHA1 follows). The expected SHA1 is stored in the **`result`** column of the `warden_checks` table.

### Files typically checked

MPQ_CHECK targets files within the game's MPQ archive chain: DBC files (Spell.dbc, Item.dbc), ADT map files, M2 model files, BLP textures, and other game data. The MPQ load order for 3.3.5a starts with `common.MPQ` and ends with locale-specific patch files. Custom `patch-{letter}.MPQ` files with higher priority are a common modification vector, making them natural check targets.

### Spoofing considerations

If no MPQ files have been modified, the client will naturally return correct SHA1 hashes — **pass-through works without intervention**. If files are modified (custom patches), spoofing requires either: (1) hooking the SFile functions to redirect reads to original unmodified data, or (2) pre-computing and caching correct SHA1 values from a clean client. Caching is viable because MPQ_CHECK uses no seed — the hash is deterministic for a given file. However, the server could check different files over time, so the cache must cover all potential targets. Hooking `SFileOpenFileEx`/`SFileReadFile` to read from backup MPQ archives is the more robust approach.

---

## PROC_CHECK: function prologue integrity via local HMAC comparison

### Client-side algorithm

The Warden module detects API hooking/detouring by verifying that specific function prologues have not been modified. The execution flow at **module offset `0xE0`** (`CallProcCheckHashAndCompare`):

1. **`GetModuleHandleA(moduleName)`** — resolves the DLL base address (handles ASLR automatically at runtime)
2. **`GetProcAddress(hModule, procName)`** — resolves the function entry point
3. **Direct memory read** of `Length` bytes starting at `funcAddress + Offset` (no `ReadProcessMemory` needed — the Warden module runs in-process)
4. **HMAC-SHA1(seed, readBytes)** — hashes the read bytes using the provided seed as key
5. **Compare** the computed HMAC against the provided SHA1 from the check request
6. **Return `0xE9`** if the HMAC matches (function is clean), or a different byte if it doesn't match

The function name from reverse engineering — `CallProcCheckHashAndCompare` — explicitly confirms this hash-and-compare pattern. **The client performs the comparison locally and returns only a pass/fail byte.** The server never sees the actual function bytes.

### Request format (server → client)

| Field | Size | Source |
|-------|------|--------|
| Type byte | 1 byte | `0x7E` (126) |
| Seed | 4 bytes (uint32) | First 4 bytes of `data` column |
| SHA1 | 20 bytes | Remaining 20 bytes of `data` column |
| moduleNameIndex | 1 byte | Index into string table (e.g., "kernel32.dll") |
| procNameIndex | 1 byte | Index into string table (e.g., "OpenProcess") |
| Offset | 4 bytes (uint32) | Offset from function entry point (`address` column) |
| Length | 1 byte | Number of bytes to read (`length` column) |

The **seed + SHA1 = `HMAC-SHA1(seed, expected_clean_bytes)`**. The seed prevents replay attacks, and the expected bytes represent the unmodified function prologue. The Offset field is **relative to the resolved function address**, not an absolute address — ASLR is irrelevant because resolution happens at runtime via string-based lookup.

### Response format

**Exactly 1 byte**, following the same handler as DRIVER_CHECK:

- **`0xE9` = pass** — HMAC matches, function prologue is unmodified
- **≠ `0xE9` = fail** — prologue has been modified (hook/detour detected), or module/function was not found

If `GetModuleHandleA` returns NULL or `GetProcAddress` fails, the behavior depends on the specific check context. For checks targeting system DLLs like kernel32.dll (which should always be loaded), a "not found" result would itself be suspicious. The server-side code treats any value ≠ 0xE9 as failure regardless of the reason.

### Functions typically checked

PROC_CHECK targets Windows API functions commonly hooked by cheat software: **`OpenProcess`**, **`NtReadVirtualMemory`**, **`ReadProcessMemory`**, **`NtQueryInformationProcess`**, **`NtSetInformationThread`**. The Length values are typically small (**5–12 bytes**) — enough to detect the common `0xE9 rel32` (5-byte near JMP) or `0xFF 0x25` (6-byte indirect JMP) inline hook patterns at function entry points.

PROC_CHECK is noted as "nyi" (not yet implemented) in some TrinityCore documentation, meaning the default database may ship without PROC_CHECK entries. Server administrators must populate their own entries with correct seed+SHA1 values computed from clean system DLL prologues.

### Third-party hook detection and spoofing

Any software that hooks checked functions will trigger PROC_CHECK failure — this includes **antivirus software**, **Discord overlay** (hooks graphics APIs), **recording software**, and other legitimate tools. The shadow copy approach (reading clean bytes from the on-disk DLL) is theoretically viable: the Warden module reads in-memory bytes, but you can hook the Warden module's memory-read function to return bytes read from the on-disk `kernel32.dll` or `ntdll.dll` instead. Multiple OwnedCore threads document detouring the Warden scanner function (pattern `74 02 F3 A5 B1 03 23 CA`) to intercept and forge scan results. The key insight is that the client only returns a 1-byte result, so the spoofing layer just needs to ensure the HMAC computation uses clean bytes.

---

## The seed + SHA1 pattern is HMAC-SHA1 for local comparison

The 24-byte `data` field in `warden_checks` stores `uint32 seed || byte[20] HMAC-SHA1(seed, target)`, confirmed by the MODULE_CHECK code path that generates these values dynamically:

```cpp
case MODULE_CHECK:
{
    uint32 seed = static_cast<uint32>(rand32());
    buff << uint32(seed);
    HmacHash hmac(4, (uint8*)&seed);  // 4-byte key
    hmac.UpdateData(wd->str);          // target string
    hmac.Finalize();
    buff.append(hmac.GetDigest(), hmac.GetLength());
}
```

For **MODULE_CHECK**, the module name is NOT sent to the client — only the HMAC. The client iterates all loaded DLLs, computes HMAC-SHA1(seed, each_dll_name), and checks for a match. This design prevents revealing what the server is looking for.

For **DRIVER_CHECK**, the driver name IS sent via string table, and the HMAC serves as request authentication (proving the check is legitimate). For **PROC_CHECK**, the HMAC represents the expected clean function bytes — the client reads actual bytes, computes HMAC-SHA1(seed, actual_bytes), and compares against the provided HMAC.

This architecture means the **client does all comparison locally** for seed-based checks. The server receives only pass/fail. The varying seed prevents caching or replaying old results for these check types (unlike MPQ_CHECK, which is deterministic).

---

## Complete response format summary

The response packet (`WARDEN_CMSG_CHEAT_CHECKS_RESULT`, opcode `0x02`) has this structure:

```
[uint16 length] [uint32 checksum] [timing_result] [check_results...]
```

The checksum is SHA1 of the payload XOR-folded into 4 bytes. Results appear in the same order checks were requested.

| Check Type | Wire ID | Response Size | Pass | Fail |
|------------|---------|---------------|------|------|
| TIMING_CHECK | `0x57` | 1 + 4 bytes | ≠ 0x00 | 0x00 |
| DRIVER_CHECK | `0x71` | **1 byte** | **0xE9** | ≠ 0xE9 |
| PROC_CHECK | `0x7E` | **1 byte** | **0xE9** | ≠ 0xE9 |
| MODULE_CHECK | `0xD9` | **1 byte** | **0xE9** | ≠ 0xE9 |
| PAGE_CHECK_A | `0xB2` | **1 byte** | **0xE9** | ≠ 0xE9 |
| PAGE_CHECK_B | `0xBF` | **1 byte** | **0xE9** | ≠ 0xE9 |
| MPQ_CHECK | `0x98` | 1 + 20 bytes | 0x00 + SHA1 | ≠ 0x00 or mismatch |
| MEM_CHECK | `0xF3` | 1 + N bytes | 0x00 + matching bytes | ≠ 0x00 or mismatch |
| LUA_EVAL | `0x8B` | 1 + 1 + N bytes | 0x00 (no string) | 0x01 + len + string |

Note the inverted convention: TIMING_CHECK uses 0x00 for fail, while DRIVER/PROC/MODULE/PAGE use 0xE9 for pass. MPQ_CHECK and MEM_CHECK use 0x00 for "OK, data follows." There is **no extended result format** for DRIVER_CHECK or PROC_CHECK — always exactly 1 byte.

---

## Server check selection and packet construction

The server selects checks from randomized pools. TrinityCore/AzerothCore organizes checks into categories: **INJECT** (DRIVER, PAGE, MODULE), **LUA** (LUA_EVAL), and **MODDED** (MPQ, MEM). Each session cycles through shuffled check IDs, selecting a configurable number per cycle (default: **3 MEM_CHECKs** + **7 other checks**, every **30 seconds**).

The request packet is built as:

1. Timing check header (`0x00` byte)
2. **String table**: all unique strings needed (driver names, module names, filenames), null-terminated
3. **Check entries**: each prefixed with its type byte, followed by type-specific data (using string indices)
4. **XOR byte**: XOR of all check type IDs (packet terminator)
5. **MEM_CHECK entries**: `0x00` + address + length (placed after the XOR byte)

The entire packet is ARC4-encrypted using session-derived keys.

---

## Warmane extends standard Warden with custom "Sentinel" system

Warmane does **not** rely solely on open-source Warden. Their custom **"Sentinel" anti-cheat** extends the standard system with additional server-side detection: Lua function unlock monitoring (detecting when protected functions become callable), DBC alteration detection, memory edit persistence tracking ("once you try a cheat, your memory remains edited for the session"), and DLL injection detection. A distinctive implementation detail: on 3.3.5a, Warden doesn't natively send Lua execution results back to the server, so Warmane reportedly uses **`SendAddonMessage`** as a backchannel for Lua-based detection results.

Sentinel bans are **automatic and non-appealable**, ranging from short suspensions (13–15 minutes for first offenses) to **30 days or permanent** for repeat violations. The system publicly announces banned players. Warmane staff explicitly refuse to document Sentinel's internals. Despite using the standard Blizzard Warden client module (same .mod file as all private servers), their server-side check definitions and response analysis go significantly beyond any open-source implementation. **No private server has ever fully utilized all of the Warden module's capabilities** according to community consensus — Warmane comes closer than most but still does not exhaust the module's functionality.

## Conclusion

Three architectural principles emerge from this analysis that matter for protocol work. First, the 0xE9 convention for seed-based checks means the server never receives raw scan data for DRIVER_CHECK or PROC_CHECK — making server-side analysis impossible beyond pass/fail. Second, the HMAC-SHA1 seed mechanism for PROC_CHECK creates a per-request unique challenge, preventing simple result caching (unlike MPQ_CHECK, which is fully deterministic). Third, the Warden module's function table (offsets 0xC8–0xF4) provides a complete map of check execution entry points, making the module's behavior fully predictable despite its obfuscated delivery mechanism. For defensive private-server protocol work, the critical edge case is PROC_CHECK: legitimate software hooks on system DLLs will cause false positives, and the correct mitigation is intercepting the Warden module's memory-read path to supply clean on-disk bytes rather than spoofing result bytes directly.