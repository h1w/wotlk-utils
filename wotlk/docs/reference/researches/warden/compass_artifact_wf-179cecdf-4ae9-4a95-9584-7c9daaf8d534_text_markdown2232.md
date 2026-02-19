# Warden MEM_CHECK spoofing: complete defensive analysis for WoW 3.3.5a

**Variant A — modifying the plaintext buffer before RC4 encryption — is the optimal approach for MEM_CHECK spoofing.** It preserves RC4 stream synchronization by design (keystream generation is plaintext-independent), gives full access to the results buffer for surgical byte replacement and checksum recalculation, and avoids the race conditions of temporary unhooking. The server compares **raw bytes via `memcmp()`**, not hashes, making spoofing straightforward once you know the expected values. This analysis is derived from direct examination of AzerothCore, TrinityCore, and MaNGOS source code, community implementations on OwnedCore, and namreeb's published research.

---

## 1. What the server actually checks for MEM_CHECK

The server's MEM_CHECK verification is a **direct raw byte comparison**, confirmed across all major open-source 3.3.5a server implementations (AzerothCore, TrinityCore, MaNGOS Two). The critical code path in `WardenWin.cpp`'s `HandleData()` works as follows:

```cpp
case MEM_CHECK:
{
    uint8 Rone;
    buff >> Rone;  // Status byte: 0x00 = memory read succeeded
    if (Rone != 0x00) {
        checkFailed = id;  // Read error = fail
        continue;
    }
    // RAW BYTE COMPARISON — not SHA1, not hash
    if (memcmp(buff.contents() + buff.rpos(),
               rs->Result.AsByteArray(0, false).get(), rd->Length) != 0) {
        checkFailed = id;  // Mismatch = fail
    }
    buff.rpos(buff.rpos() + rd->Length);
    break;
}
```

The expected bytes are stored in the `warden_checks` database table's `result` column as a hex string (e.g., `"8BFF558BEC"` for a standard `MOV EDI,EDI; PUSH EBP; MOV EBP,ESP` prologue). The server loads this hex string into a `BigNumber` via `SetHexStr()`, then extracts it as a raw byte array for `memcmp()`. **There is no salt, seed, or HMAC involved in MEM_CHECK validation** — the comparison is purely positional byte matching.

The server selects check addresses from the `warden_checks` table using a round-robin shuffle: **3 MEM_CHECKs** and **7 other checks** per cycle (AzerothCore defaults), or **9 injection checks** + **1 Lua** + **1 MPQ** (TrinityCore master). Each cycle, checks are randomly drawn from a pool without replacement until the pool is exhausted, then refilled. The `address` field in `warden_checks` specifies the memory offset (relative to a module base if `str` names a module, otherwise absolute), and `length` specifies how many bytes to read (max 255, stored as `TINYINT UNSIGNED`).

Penalty handling is **configurable** via `Warden.ClientCheckFailAction`: **0** = log only (default), **1** = kick, **2** = ban. Per-check overrides are possible via the `warden_action` table. AzerothCore adds an `_interrupted` guard that suppresses penalties when a check cycle was interrupted by `ForceChecks()`. The response **timeout is 600 seconds** (10 minutes, configurable via `Warden.ClientResponseDelay`); exceeding it always results in a kick regardless of the fail-action setting.

---

## 2. CMSG_WARDEN_DATA packet format and checksum algorithm

The CHEAT_CHECKS_RESULT packet (opcode `0x02`) has this exact structure after RC4 decryption:

```
[opcode:    1 byte  = 0x02]          ← already consumed before HandleData()
[resultLen: 2 bytes LE (uint16)]     ← length of the results section
[checksum:  4 bytes LE (uint32)]     ← SHA1-XOR-folded checksum
[results:   resultLen bytes]         ← all check results concatenated
```

The results section begins with a **TIMING_CHECK result** (always first: `[uint8 result][uint32 clientTicks]`), followed by each requested check's result in the same order as `_CurrentChecks`. For MEM_CHECK specifically:

- **Success**: `[0x00][raw_bytes: readLen bytes]` — status byte zero followed by raw memory contents
- **Failure**: `[non-zero error code]` — single byte, no memory data follows

The **checksum algorithm** (`BuildChecksum` in `Warden.cpp`) is SHA1 XOR-folded to `uint32`:

```cpp
uint32 Warden::BuildChecksum(uint8 const* data, uint32 length) {
    keyData hash;
    hash.bytes = Trinity::Crypto::SHA1::GetDigestOf(data, size_t(length));
    uint32 checkSum = 0;
    for (uint8 i = 0; i < 5; ++i)
        checkSum = checkSum ^ hash.ints[i];  // XOR 5 uint32_t LE words
    return checkSum;
}
```

The checksum covers the **entire results section** (`resultLen` bytes starting after the checksum field). This means any modification to any byte within the results section — whether a single MEM_CHECK result or multiple — invalidates the checksum and requires full recalculation over all `resultLen` bytes.

---

## 3. RC4 stream cipher: persistent state and synchronization guarantees

Warden uses **one persistent RC4 state per direction per session**, not per-packet reinitialization. The server maintains `_inputCrypto` (for decrypting client→server) and `_outputCrypto` (for encrypting server→client) as `ARC4` objects initialized once during `WardenWin::Init()` with 16-byte keys derived from the SRP6 session key. After the module hash verification (`HandleHashResult`), these are re-keyed with module-specific seeds, but the fundamental property remains: **a single continuous keystream per direction**.

```cpp
void Warden::DecryptData(uint8* buffer, uint32 length) {
    _inputCrypto.UpdateData(buffer, length);  // advances RC4 state by `length` bytes
}
```

This architecture has three critical implications for each spoofing variant:

**Variant A (modify plaintext before RC4)** is inherently synchronization-safe. RC4's PRGA generates keystream bytes `K₀, K₁, K₂, ...` that depend exclusively on the internal S-box state and indices — **the keystream is completely independent of plaintext content**. Changing the plaintext from `P` to `P'` before the original function calls `RC4_encrypt(buffer, len)` produces `P' ⊕ K` instead of `P ⊕ K`, but the RC4 state advances by exactly `len` bytes regardless. Both client and server process the same number of bytes, so their streams remain synchronized.

**Variant B (XOR-patch ciphertext post-encryption)** is mathematically correct: `new_cipher = old_cipher ⊕ old_plain ⊕ new_plain = (old_plain ⊕ K) ⊕ old_plain ⊕ new_plain = new_plain ⊕ K`. The RC4 state is completely unaffected because no RC4 function is invoked — only XOR operations on already-produced ciphertext. However, this requires access to both old plaintext and old ciphertext simultaneously, which is architecturally more complex.

**Desynchronization occurs if and only if the byte count changes.** Adding, removing, or dropping bytes causes the client and server RC4 states to diverge permanently, corrupting all subsequent packets. Content-only modifications at identical length never cause desync.

---

## 4. Comparative analysis of the four interception approaches

### Variant A: Modify plaintext before RC4 encryption — RECOMMENDED

Hook the RC4 PRGA call inside the Warden module that encrypts the outgoing CMSG_WARDEN_DATA payload. The hook sees `dataPtr` (plaintext buffer) and `length` before the original function encrypts it. At this point, the complete plaintext of all results is assembled and the checksum is already computed.

**Strengths**: Full plaintext access enables surgical modification of specific MEM_CHECK bytes and correct checksum recalculation. RC4 synchronization is guaranteed by design. No race conditions. The original function encrypts the spoofed data transparently.

**Algorithm**:
1. Parse the plaintext buffer to locate the results section (skip opcode byte, read `resultLen` at offset 1, read `checksum` at offset 3)
2. Walk the results section matching each check against the server's request order (you must track which checks were requested, from the inbound SMSG_WARDEN_DATA)
3. For each MEM_CHECK whose address overlaps your hooks, overwrite the `readLen` bytes with the original (clean) bytes
4. Recalculate the checksum: `SHA1(modified_results, resultLen)` → XOR-fold 5 uint32s
5. Write the new checksum at offset 3 in the buffer
6. Return to the original RC4 encrypt function, which encrypts the modified buffer

**Pitfalls**: Requires finding and hooking a function inside the dynamically-loaded Warden module, whose base address changes per session. The Warden module is downloaded, RSA-verified, and mapped at runtime. Pattern scanning (e.g., signature `74 02 F3 A5 B1 03 23 CA` or `f3 a4 5f 5e c3`) is needed each time.

### Variant B: XOR-patch in SendPacket hook

Intercept `CMSG_WARDEN_DATA` in the WoW client's `SendPacket` path after RC4 encryption. Apply `new_cipher[i] = old_cipher[i] ⊕ old_plain[i] ⊕ new_plain[i]` for modified bytes.

**Strengths**: Hook point is in the static WoW executable (stable address). No need to find Warden module internals.

**Pitfalls**: Requires retaining a copy of the original plaintext before encryption to compute the XOR delta. This means you need *two* interception points — one to capture plaintext, one to patch ciphertext — defeating the simplicity advantage. The checksum is also encrypted, so you must XOR-patch its 4 bytes too. If you already have plaintext access, Variant A is strictly simpler.

### Variant C: Modify internal buffer between assembly and encryption

Find the Warden module's internal result buffer after individual check results are written but before the final packet is assembled.

**Strengths**: Could intercept at a higher semantic level (per-check rather than per-packet).

**Pitfalls**: The Warden module's internal data structures are undocumented and change between module versions. Finding the exact buffer between assembly and encryption requires deep reverse engineering of each module version. Much more fragile than Variant A.

### Variant D: Temporary unhook — restore original bytes during scan

Before the Warden module reads memory at a hooked address, restore the original bytes; after the read completes, reinstall the hook.

**Strengths**: Conceptually simple. No packet modification needed — the scan naturally produces clean results, so the checksum is automatically valid.

**Pitfalls**: **Race conditions are the critical concern.** While the Warden module runs on the main WoW thread (confirmed by community analysis), your hooked code also executes on the main thread during the restore window. If the restored bytes include function prologues, any call to those functions during the unhook window will execute unhooked code. For multi-byte unhook/rehook operations, you must also consider instruction atomicity — partially written instructions can cause crashes if an interrupt or exception handler fires mid-write. Additionally, overlapping Warden checks (confirmed by community member "Jadd": addresses `0x8B5BEF` length 9 and `0x8B5BF6` length 5 overlap) require merging address ranges into consolidated restore regions.

### Verdict

**Variant A is optimal.** It has the strongest correctness guarantees (RC4-safe by construction), requires only a single interception point, provides full plaintext access for both modification and checksum recalculation, and has no race conditions. The only complexity is locating the hook point in the dynamic Warden module, which is a solved problem (pattern scanning).

---

## 5. MinHook inline hook patch sizes and implications

For **32-bit x86** (WoW 3.3.5a is a 32-bit process), MinHook uses a **5-byte `JMP rel32`** (`E9 xx xx xx xx`). This covers the entire 4 GB address space because overflow bits are ignored in x86 relative address arithmetic — no relay function is needed.

The actual number of bytes overwritten at the hook point depends on **instruction alignment**:

- MinHook uses HDE (Hacker Disassembler Engine) to disassemble instructions starting at the target address
- It accumulates complete instructions until the total length ≥ 5 bytes
- It copies these complete instructions to the trampoline and overwrites the originals with the JMP + NOP padding

For example, if a function starts with `SUB ESP, 0x10` (3 bytes) + `PUSH EBX` (1 byte) + `PUSH ESI` (1 byte), MinHook overwrites exactly 5 bytes. But if it starts with `MOV EDI, EDI` (2 bytes) + `PUSH EBP` (1 byte) + `MOV EBP, ESP` (2 bytes), the 5-byte JMP covers exactly these instructions. If the function starts with a single 6-byte instruction, MinHook must overwrite all 6 bytes.

**For Warden spoofing, this matters because you must know exactly which bytes changed at each hook address.** To determine the precise patch size programmatically:

1. After calling `MH_CreateHook()`, inspect MinHook's internal `HOOK_ENTRY` structure — the `patchAbove` flag and the trampoline's `oldIPs[nIP-1]` value give the exact byte count
2. Alternatively, disassemble instructions at the target address yourself using a length disassembler until cumulative length ≥ 5
3. Store the original bytes before `MH_EnableHook()` — compare post-enable to determine exactly which bytes changed

**"patchAbove" mode**: If the target function starts with `MOV EDI, EDI` (0x8BFF, the Windows hot-patching prologue) and there are 5 bytes of NOP/INT3 padding immediately above, MinHook writes a 5-byte JMP in the padding and only a **2-byte short JMP** (`EB F9`) at the function entry. This minimizes the footprint to just 2 bytes at the actual function address.

---

## 6. Existing open-source implementations and community approaches

The most widely documented approach in the community is **hooking the Warden module's memory scan function** (the `repe movsb` / `rep movsb` instruction that copies scanned memory bytes). This is effectively a variant of Variant A/D — intercepting at the point where memory is read, before results are assembled.

**OwnedCore "[Howto] Bypassing Warden" thread** describes the canonical technique: hook `LoadWardenModule` to capture the Warden module's base address via the `VirtualAlloc` call, then pattern-scan for the scan function signature (`74 02 F3 A5 B1 03 23 CA` or `f3 a4 5f 5e c3`), and hook the `repe movsb` instruction. When the scan targets an address overlapping your hooks, substitute clean bytes in the source buffer. This avoids all RC4 and checksum concerns because the scan naturally produces correct results.

**namreeb's research** (github.com/namreeb) includes WardenSigning (analysis of 72 sniffed Warden modules and their RSA signatures) and wowreeb (a launcher using hadesmem for injection). namreeb documented the recommended approach on OwnedCore: "Hook the function which performs [the scan], determine if the affected area overlaps with your code, and if so, modify the reply accordingly." His work confirms that the scan function approach is the standard in the community.

**Jordan Whittle's research** (jordanwhittle.com/posts/exploiting-warden/) provides a complementary technique: using data breakpoints to discover which addresses Warden scans, then modifying *adjacent unscanned bytes* to achieve the same effect without triggering detection — for example, replacing `TEST EAX, EAX` with `XOR EAX, EAX` at a nearby unscanned address rather than modifying the scanned one.

**SpellFire** (github.com/k-kowalski/SpellFire) claims Warden protection for WoW 3.3.5a build 12340, though its specific implementation details were not publicly documented in sufficient detail.

The **"disable WardenClient_HandlePacket" approach** (patching address `0x006CA5C0` with a `RET`) is documented as the simplest bypass but fails on any server with response-timeout checking, which is enabled by default in all modern cores.

---

## 7. Checksum recalculation in practice

When spoofing MEM_CHECK results within a CHEAT_CHECKS_RESULT packet, the checksum covers the **entire results section** as a single block. You cannot recalculate a partial checksum — any byte change in any check result requires recomputing `SHA1(all_results)` and XOR-folding.

In **Variant A**, this is straightforward because you have the complete plaintext buffer:

```
Buffer layout at hook point:
[0x02] [resultLen: 2B] [checksum: 4B] [results: resultLen bytes]
 ^                       ^              ^
 offset 0               offset 3       offset 7
```

**Recalculation procedure**:
1. Locate results at `buffer + 7` (after opcode + resultLen + checksum)
2. Modify the specific MEM_CHECK bytes within results
3. Compute new checksum: `SHA1(buffer + 7, resultLen)` → reinterpret as 5 × `uint32_t` LE → XOR all five → write result at `buffer + 3`

In **Variant B**, the checksum bytes are encrypted. You must XOR-patch both the modified MEM_CHECK bytes *and* the 4 checksum bytes using the `new_cipher = old_cipher ⊕ old_plain ⊕ new_plain` formula. This requires knowing the old plaintext checksum value, which means you need to have captured the plaintext before encryption — again pointing toward Variant A as the simpler approach.

One important implementation detail: to walk the results section and find the correct offset for a specific MEM_CHECK result, you must **track the order and types of checks in the current request**. This requires also intercepting the inbound SMSG_WARDEN_DATA (opcode `0x02`) to parse which checks were requested and in what order. Each check type has a different result size: MEM_CHECK = 1 + readLen bytes (if status = 0x00), PAGE_CHECK/MODULE_CHECK/DRIVER_CHECK = 1 byte, MPQ_CHECK = 1 + 20 bytes, TIMING_CHECK = 1 + 4 bytes.

---

## 8. Race conditions, threading, and timing constraints

**Threading**: The Warden client module executes on the **WoW main thread**. Memory scans are performed synchronously within the packet handler when SMSG_WARDEN_DATA arrives. There is no separate Warden scan thread. This eliminates most race condition concerns for Variant D (temporary unhook), but does not eliminate all of them — interrupt handlers, exception handlers, and Windows APCs can still execute during the unhook window.

**Variant D race window analysis**: The unhook-rehook cycle involves: (1) write original bytes over hook, (2) let Warden scan, (3) rewrite hook bytes. Steps 1 and 3 each require writing 5+ bytes. On x86, a 5-byte write is **not atomic** — it requires multiple store instructions. If a hardware interrupt fires between stores, and the interrupt handler (or DPC) calls the partially-restored function, a crash results. The probability is extremely low but nonzero. Using `VirtualProtect` + `memcpy` + `FlushInstructionCache` adds latency. In contrast, Variant A avoids this entirely — hooks remain in place at all times.

**Server-side timeouts**: The response timeout defaults to **600 seconds** (configurable via `Warden.ClientResponseDelay`). This is extremely generous — normal Warden response processing takes milliseconds. Any spoofing approach that adds less than a second of processing time will never trigger this timeout. The check request interval defaults to **30 seconds** (`Warden.ClientCheckHoldOff`), meaning new check cycles arrive roughly every 30 seconds.

**AzerothCore interrupt handling**: AzerothCore added protection against check-cycle interruption. If `ForceChecks()` or `RequestChecks()` is called while a response is pending (`_dataSent == true`), it sets `_interrupted = true`. The penalty at the end of `HandleData()` is gated by `if (checkFailed > 0 && !_interrupted)`, preventing false positives from race-condition overlapping check cycles server-side.

---

## Recommended architecture and detailed algorithm

The recommended implementation consists of three components:

**Component 1: Hook registry.** Maintain a map of all addresses you've hooked: `map<uintptr_t, HookRecord>` where `HookRecord` stores `{original_bytes[], patch_bytes[], length}`. When MinHook creates a hook, record the original bytes and the overwritten byte count (determined by disassembling instructions at the target until cumulative length ≥ 5).

**Component 2: Inbound request parser.** Hook the Warden module's packet receive path (or the RC4 decrypt function for SMSG_WARDEN_DATA) to parse incoming check requests. Extract the ordered list of `{check_type, address, readLen}` tuples for each MEM_CHECK. Store this as `_pendingChecks` for use when processing the outbound response.

**Component 3: Outbound response modifier (Variant A).** Hook the RC4 encrypt call for the outbound CMSG_WARDEN_DATA. When the hook fires:

1. Check if buffer[0] == 0x02 (CHEAT_CHECKS_RESULT)
2. Parse `resultLen` from buffer[1..2] and `checksum` from buffer[3..6]
3. Walk the results section starting at buffer[7]:
   - Skip TIMING_CHECK result (1 + 4 bytes)
   - For each check in `_pendingChecks`:
     - If MEM_CHECK with status 0x00: check if `[address, address+readLen)` overlaps any entry in the hook registry
     - If overlap found: copy the original bytes from the hook registry into the results buffer at the correct offset
     - Advance offset by 1 + readLen (or 1 if status != 0x00)
     - For non-MEM_CHECK types: advance by the appropriate fixed size
4. If any bytes were modified: recalculate checksum via `SHA1(buffer + 7, resultLen)` → XOR-fold → write to buffer[3..6]
5. Return to original RC4 encrypt function

This approach is **deterministic, race-free, and RC4-synchronization-safe**. The primary risks are: (a) incorrect parsing of the results section due to mismatched check ordering, which would corrupt unrelated check results and trigger detection; (b) Warden module updates that change the location of the RC4 encrypt call, requiring updated pattern signatures; and (c) PAGE_CHECK_A/B checks that use SHA1 hashing of memory pages — these cannot be spoofed by replacing individual bytes, as the hash is computed client-side over a page-sized region. For PAGE_CHECK, you must ensure your hooks don't land in pages that are PAGE_CHECK targets, or use the Warden scan function hook approach to intercept at the memory-read level instead.

---

## Conclusion

The server-side implementation across AzerothCore, TrinityCore, and MaNGOS is remarkably consistent: **`memcmp` of raw bytes** for MEM_CHECK, **SHA1 XOR-fold** for packet checksums, **persistent RC4** streams per session, and **configurable penalties** (default: log-only). Variant A — modifying the plaintext buffer before the Warden module's own RC4 encryption — emerges as the optimal approach because it inherits RC4 synchronization for free, provides full plaintext access for both byte replacement and checksum recalculation, and avoids the atomicity hazards of temporary unhooking. The community consensus, reflected in namreeb's recommendations and the OwnedCore knowledge base, favors intercepting at the scan function level (a close relative of Variant A), with the alternative of avoiding scanned addresses entirely by patching nearby unscanned bytes. For a production defensive implementation on a private server, combining the hook registry pattern with the outbound response modifier provides robust, maintainable MEM_CHECK spoofing with well-understood failure modes.