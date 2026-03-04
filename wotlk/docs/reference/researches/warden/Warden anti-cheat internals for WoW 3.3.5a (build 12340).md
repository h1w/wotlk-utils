# Warden anti-cheat internals for WoW 3.3.5a (build 12340)

**Warden modules in 3.3.5a are purely data-gathering agents that run synchronously on WoW's main thread, use SHA1/HMAC-SHA1 (not CRC32) for integrity hashing, and report raw scan results to the server — which alone decides penalties.** The module does not independently trigger the "Unauthorized software" popup; it simply returns memory bytes and hashes. Self-integrity protection exists in two forms: a `.data`-segment trap that catches naive bypass attempts, and a local-only code-start check on some module builds that crashes (but does not ban) when the scan function is hooked. Hardware breakpoint detection, VEH enumeration, and trampoline scanning were **not** present in the 3.3.5a-era module. All check processing — including HASH_REQUEST — is synchronous on the main thread with no worker thread or deferred computation.

---

## SHA1 dominates integrity checking, not CRC32

Warden's 3.3.5a modules use **SHA1 and HMAC-SHA1** as the primary integrity-verification algorithms. CRC32 is not used for any scan check type. The full cryptographic stack:

- **HMAC-SHA1** with a server-provided 4-byte seed for PAGE_CHECK_A (opcode `0xB2`) and PAGE_CHECK_B (`0xBF`). The server sends `uint32 Seed + byte[20] ExpectedSHA1 + uint32 Address + byte Length`. The module computes HMAC-SHA1 over the specified memory region and the client returns pass/fail. The seed prevents replay attacks — each check cycle uses different seeds, so cached responses fail.
- **Raw byte reads** for MEM_CHECK (`0xF3`), the most frequently used check. The module reads N bytes at a specified address and returns them verbatim. The server compares against known-good values. This is how inline hooks are caught: the server stores expected prologue bytes (e.g., `55 8B EC` for `push ebp; mov ebp, esp`) and flags any deviation.
- **SHA1** for MPQ_CHECK (`0x98`), computing file hashes of game archives.
- **MD5** exclusively for module identification — each Warden module is identified by the MD5 of its compressed data.
- **RSA 2048-bit** for module signature verification during loading. The module binary ends with a `"SIGN"` magic marker plus a 256-byte signature, verified against Blizzard's hardcoded public key (exponent `0x10001`, same modulus across 1.12.1, 2.4.3, and 3.3.5a). The expected plaintext is `SHA1(module_data + "MAIEV.MOD")` padded with `0xBB`.

The module itself **never makes detection decisions**. It collects data and reports it. The server-side comparison logic is what triggers penalties. This is a critical architectural detail — the intelligence lives server-side while the module is a dumb scanner.

### The `.data` segment trap

One particularly clever self-protection mechanism targets naive bypass attempts. Warden scans an address in the WoW binary's `.data` segment — specifically **address `0x00AC3DAC`** (8 bytes) — whose runtime value differs from its on-disk value. On disk, the DWORD at this address is `0xFFFFFFFF`. At runtime, it changes based on login state (values 0–17, with the expected scan result being `04000000903C9F00` for the logged-in state). The writes are:

```asm
.text:004D80E2  mov dword_AC3DAC, eax  ; eax = login state (0..17)
.text:004D8AA5  mov dword_AC3DAC, ecx  ; ecx = -1 (reset)
```

Anyone who bypasses Warden by redirecting all memory reads to the on-disk executable will return `FFFFFFFF` instead of `04000000` and get flagged. As OwnedCore researcher Jadd explained: "Warden validates it with the 'logged in' bytes, so if you replace this memcpy result with bytes in the executable file, you will flag yourself for a ban." Redirecting `.text` and `.rdata` reads to the disk file is safe because those segments are read-only and identical at runtime, but the `.data` trap specifically catches this shortcut.

---

## Everything runs synchronously on the main thread

Warden in 3.3.5a has **no worker thread**. All scan processing — packet handling, memory reads, hash computation, and response generation — executes synchronously on WoW's main thread. This is confirmed by multiple independent observations:

**The module exports exactly four functions** through its callback table:

| Export | Signature | Purpose |
|--------|-----------|---------|
| `fpGenerateRC4Keys` | `void (__thiscall*)(WardenFuncList**, void*, DWORD)` | Derive new RC4 keys from server seed |
| `fpUnload` | `void (__thiscall*)(WardenFuncList**)` | Cleanup; stores RC4 state before freeing |
| `fpPacketHandler` | `void (__thiscall*)(WardenFuncList**, BYTE*, DWORD, DWORD*)` | Handle incoming `SMSG_WARDEN_DATA` packets |
| `fpTick` | `void (__thiscall*)(WardenFuncList**, DWORD)` | Called from main game loop; DWORD = elapsed ms |

The `fpTick` function is invoked from WoW's main game loop each frame, receiving the tick delta. The `fpPacketHandler` processes `SMSG_WARDEN_DATA` synchronously when the packet arrives on the main thread. Debug registers (DR0–DR3) must be set on WoW's main thread to intercept scans because DRs are thread-local. The EndScene hook (D3D rendering callback) was the canonical execution context for Warden interaction precisely because it runs on the same main thread.

**Implications for timing-based evasion**: Since check processing is synchronous, a hook that is removed before the main thread processes the check packet and reinstalled afterward could theoretically evade detection — but the timing window is narrow and non-deterministic.

---

## Disassembly patterns of the scan routine

The Warden module's core memory scanner is a `cdecl` function with a well-documented signature. Multiple researchers independently identified it:

**Function prototype:**
```c
char* __cdecl copyBytesForScan(char *dest, const char *source, unsigned int len)
```

**Full byte pattern** (37 bytes, stable across sessions):
```
56 57 FC 8B 54 24 14 8B 74 24 10 8B 44 24 0C
8B CA 8B F8 C1 E9 02 74 02 F3 A5 B1 03 23 CA
74 02 F3 A4 5F 5E C3
```

This disassembles to:
```asm
push esi
push edi
cld
mov  edx, [esp+14h]    ; len
mov  esi, [esp+10h]    ; source address being scanned
mov  eax, [esp+0Ch]    ; dest buffer
mov  ecx, edx
mov  edi, eax
shr  ecx, 2
jz   short skip_dwords
repe movsd              ; copy DWORDs
skip_dwords:
mov  cl, 3
and  ecx, edx
jz   short done
repe movsb              ; copy remaining bytes
done:
pop  edi
pop  esi
ret
```

The **5-byte unique signature** `F3 A4 5F 5E C3` (`repe movsb; pop edi; pop esi; ret`) is sufficient to locate this function after module load. During execution, **ESI holds the address being scanned** — setting a hardware breakpoint on a known scanned address and checking ESI at the `repe movsb` instruction reveals all scan targets.

The function's offset within the Warden module varies by build: **`0x79D8`**, **`0x668A`**, and **`0x10C5`** have all been observed across different module versions. A higher-level wrapper function exists:

```c
int __stdcall WardenInterface::ReadRelativeAddress(
    char *dest, uintptr_t baseAddr, uintptr_t addr, size_t len)
```

This wrapper is called exclusively during MEM_CHECK scans (the lower-level `copyBytesForScan` is used for other internal operations too), making it a cleaner hook target for selective bypass.

### Local self-check on the scan function

Some Warden module builds include a **local integrity check on the first bytes of the scan function**. If those bytes are overwritten (e.g., by an inline JMP hook), the module **crashes the client** rather than reporting the modification to the server. OwnedCore researcher Jadd confirmed: "If you hook the start of the 'scan function' you may experience a crash, because on some (not all) clients they do check this (but the result is not sent back to the server)." This is a tamper-and-kill mechanism, not a detection-and-report mechanism. The FireHack developer independently confirmed this behavior. The check is not present on all module builds, suggesting it was added iteratively.

---

## Detection signaling and the "Unauthorized software" popup

The Warden module **never directly triggers UI popups or Lua events**. The signaling pathway is:

1. Module performs scans and returns raw results via the `fpSendPacket` callback (provided by the client during initialization), which sends encrypted `CMSG_WARDEN_DATA` packets to the server.
2. Server deserializes the response, iterating through each check result. For MEM_CHECK: compares returned bytes against expected values. For PAGE_CHECK: checks the pass/fail status byte (`0x00` = match, anything else = mismatch). For MODULE_CHECK: `0x00` = not found (pass), `0x01` = found (fail).
3. Server calls `ApplyPenalty()` which executes the configured action — `WARDEN_ACTION_LOG` (silent), `WARDEN_ACTION_KICK` (disconnect), or `WARDEN_ACTION_BAN` (account ban).

**The "Unauthorized software or modifications detected" message is triggered by the server disconnecting the client.** The WoW client displays this popup in response to a specific disconnect reason code. On retail Blizzard servers, Warden detections were typically accumulated silently and acted on in delayed ban waves — not immediate kicks. Private servers like Warmane using TrinityCore/AzerothCore generally configure immediate kicks or bans through the `Warden.ClientCheckFailAction` config option.

The module also receives client function addresses during initialization that enable specific scan capabilities:

- **`SFileOpenFile` / `SFileReadFile` / `SFileCloseFile`** (at offsets `0x002485F0`, `0x00248460`, `0x00248730` relative to `0x00400000`) — for MPQ file integrity checks
- **`FrameScript::GetText`** (at `0x00419D40`) — for Lua string/variable checks
- **`PerformanceCounter`** (at `0x0046AE20`) — for timing checks

---

## Hook detection beyond code integrity is limited in 3.3.5a

The 3.3.5a Warden module's detection capabilities beyond byte-level memory scanning are **significantly more limited** than many assume. Here is the status of each technique:

**VEH (Vectored Exception Handler) detection: Not present.** The 3.3.5a module does not enumerate `LdrpVectorHandlerList` or check PEB `CrossProcessFlags` for VEH presence. VEH-based hooking (PAGE_GUARD + exception handler to redirect execution without modifying function bytes) was specifically designed to evade Warden's byte-integrity checks, and it worked. NCC Group's security research notes that "VEH use is a well known technique in the gaming community as a means of bypassing integrity checking code."

**Hardware breakpoint (DR0–DR3) detection: Not present.** Warden did not call `GetThreadContext()` to inspect debug registers during the 3.3.5a era. OwnedCore researcher Cypher confirmed that "DR hooks are safe" after the LuaNinja banwave. The LuaNinja detection was accomplished by scanning memory to find the injected module, not by detecting DR register usage. As amadmonk noted: "If they're checking for DR breakpoints in the code segment, they're going to start banning a bunch of people who are debugging WoW."

**NtQueryVirtualMemory for nearby allocations: Bypassed, not detected.** When Cypher's protection for LuaNinja hooked `NtQueryVirtualMemory` to hide injected modules, Blizzard's response was to **bypass the hook** by modifying the WoW client code path — not to detect the hook itself. Cypher stated: "Rather than detect the DR hooks they added code to the client (which then indirectly modified Warden) to bypass my hook on NtQueryVirtualMemory." The bypass was "general enough to bypass all usermode NtQueryVirtualMemory hooks." Modern retail WoW escalated to **direct syscalls** to avoid usermode hooks entirely.

**MinHook trampoline / JMP stub scanning: Not explicit.** Warden does not scan for JMP stubs in nearby memory or use heuristics to detect trampolines. Detection of inline hooks is purely through MEM_CHECK byte comparison at specific known addresses. If your hook target isn't in the scan list, it's invisible to Warden. Jordan Whittle demonstrated that all scanned addresses can be enumerated by hooking the scanner and dumping ESI values.

**What Warden does detect beyond code integrity:**

- **MODULE_CHECK** (`0xD9`): Uses `CreateToolhelp32Snapshot` / `Module32First` / `Module32Next` to enumerate all loaded DLLs and match against HMAC-SHA1 signatures of known cheat modules.
- **DRIVER_CHECK** (`0x71`): Detects loaded kernel drivers (32-bit only).
- **PAGE_CHECK_A** (`0xB2`): Scans **all virtual memory pages** via `VirtualQuery`, which could theoretically detect executable allocations near module memory — but only for addresses the server specifically targets.
- **TIMING_CHECK** (`0x57`): Detects `GetTickCount()` detouring by comparing client-reported ticks against server expectations.
- **Anti-debug** (via `battle.net.dll`, not Warden module): `NtQueryInformationProcess` with `ProcessDebugObjectHandle`, rogue exception vectors, `NtSetInformationThread`. These run during login, not as Warden module checks.

---

## HASH_REQUEST is fully synchronous with no deferred computation

The HASH_REQUEST mechanism is straightforward and synchronous. After the module loads, the server sends `WARDEN_SMSG_HASH_REQUEST` (opcode `0x05`) containing a **16-byte seed**. The module immediately computes an SHA1 hash of this seed using its internal key material and returns the 20-byte result via `WARDEN_CMSG_HASH_RESULT` (opcode `0x04`).

The server verifies the result against its precomputed `ClientKeySeedHash` (stored in the module definition). On success, both sides rotate to new RC4 encryption keys (`ClientKeySeed` for client→server, `ServerKeySeed` for server→client), and `_initialized` is set to `true`. Only then does the server begin sending `WARDEN_SMSG_CHEAT_CHECKS_REQUEST` packets.

**There is no deferred or asynchronous hash computation.** The entire flow — receive seed, compute SHA1, send response, rotate keys — happens synchronously on the main thread within the `fpPacketHandler` callback. The state machine progression is:

```
STATE_INITIAL → MODULE_USE sent → STATE_REQUESTED_MODULE
→ MODULE_OK received → HASH_REQUEST sent → STATE_REQUESTED_HASH
→ HASH_RESULT received → HandleHashResult() → InitializeModule() → STATE_REQUESTED_DATA
→ Periodic CHEAT_CHECKS_REQUEST / CHEAT_CHECKS_RESULT cycles
```

**Deferred hook installation after HASH_REQUEST is safe from the HASH_REQUEST itself** — the hash is computed and sent before any cheat checks begin. However, the check cycle starts immediately after initialization completes (default **10-second intervals** in emulators, ~60 seconds on retail), so hooks must be installed with awareness of the first `CHEAT_CHECKS_REQUEST` timing. The check timer in MaNGOS starts at `_checkTimer(10000)` (10 seconds).

---

## Conclusion

The 3.3.5a Warden module is architecturally simpler than many assume. It is a **single-threaded, synchronous data collector** with no independent decision-making authority. Its self-protection relies on the `.data` trap (catching naive disk-redirect bypasses) and an inconsistent local crash-on-tamper check at the scan function entry point — not sophisticated anti-tamper like modern game anti-cheats. The module uses **SHA1/HMAC-SHA1** for integrity hashing, not CRC32. It does **not** detect VEH handlers, hardware breakpoints, or nearby trampoline allocations. All scan processing runs on the main thread with no worker thread or async computation. The most significant practical insight is that the module only scans addresses **explicitly requested by the server** — it has no autonomous scanning capability. Hooking the `ReadRelativeAddress` wrapper or the lower-level `copyBytesForScan` function (with awareness of the local integrity check on some builds) and returning clean bytes for targeted addresses remains the canonical bypass approach, as Cypher noted: "Two simple function hooks are all that's needed."