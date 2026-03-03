# Task: Stealth RC4 Hooking (Hardware Breakpoints + VEH)

> **Status**: TODO
> **Created**: 2026-03-03
> **Phase**: Warden Anti-Detection
> **Depends on**: nothing
> **Blocks**: nothing

---

## Table of Contents

1. [Goal](#goal)
2. [Problem Statement](#problem)
3. [Root Cause Analysis](#root-cause)
4. [Research Summary](#research-summary)
5. [Architecture: Current vs New](#architecture)
6. [Phase 1: Hardware Breakpoints + VEH](#phase-1)
7. [Phase 2: KSA False Positive Filtering](#phase-2)
8. [Phase 3: Defensive NtGetContextThread Hook](#phase-3)
9. [Phase 4: Scan Function Output Buffer Hooking (Optional)](#phase-4)
10. [Technical Reference](#technical-reference)
11. [Files Inventory](#files-inventory)
12. [Implementation Order](#implementation-order)
13. [Acceptance Criteria](#acceptance-criteria)

---

<a name="goal"></a>
## 1. Goal

Replace MinHook-based inline JMP patches (5-byte `0xE9`) inside the Warden module with **hardware breakpoints (DR0-DR3) + Vectored Exception Handler (VEH)**, achieving **zero code modification** in Warden's memory region. This eliminates detection by MEM_CHECK, PAGE_CHECK, and any self-integrity verification.

Key requirements:
- **Zero bytes modified** in Warden module memory (no JMP, no INT3, no code changes)
- **Preserve existing spoof flow**: plaintext must be captured and modified BEFORE RC4 encryption
- **Support up to 4 simultaneous hooks** (matching 4 debug registers DR0-DR3)
- **Thread-safe**: debug registers are per-thread, must be applied correctly
- **Same API surface**: `Install()`, `Remove()`, `IsActive()`, `ConsumePlaintext()` preserved for hooks.cpp integration
- **Deferred install timing preserved**: hooks installed after HASH_REQUEST, removed on MODULE_USE

---

<a name="problem"></a>
## 2. Problem Statement

### 2.1 Incident (2026-03-02)

During a session on a WoW 3.3.5a private server, after ~4 minutes of normal operation, the message **"Unauthorized software or modifications to your game client have been detected. You will be disconnected shortly."** appeared 3 times in red chat, followed by disconnect.

### 2.2 Log Analysis (wotlk_20260302_211842.log, 4875 lines)

Timeline of key events:
1. First Warden module loaded and operated normally
2. Second module **FC6560DA** loaded (never seen before)
3. 3 RC4 hooks installed via MinHook at module+0x1217, +0x293a, +0x5bd2
4. Hook #2 at +0x293a identified as **KSA** (Key Schedule Algorithm), not PRGA — useless hook, extra detection surface
5. All CHEAT_CHECKS_RESULT checksums were **VALID** (server-side validation passed)
6. No MEM_CHECK addresses in the log directly targeted our hook addresses
7. `MH_DisableHook` / `MH_RemoveHook` failed with `MH_ERROR_MEMORY_PROTECT` when old module unloaded (memory already freed)
8. Type 0x22 remained unknown throughout entire second module session
9. Disconnect occurred ~4 minutes after module load

### 2.3 What Didn't Cause It

- NOT server-side check mismatch (all checksums valid)
- NOT MEM_CHECK at our specific hook addresses (not in scan list for this session)
- NOT RC4 re-key desync (HASH_RESULT was not modified)

### 2.4 What Likely Caused It

The 5-byte MinHook JMP (E9) patches at 3 locations inside the Warden module were detected by one of:
1. **PAGE_CHECK**: server sends `HMAC-SHA1(seed, memoryPage)` for the page containing our hooks — hash mismatch
2. **Self-integrity check** (less likely in 3.3.5a): module hashes its own code and the JMP patches corrupt the result
3. **Future MEM_CHECK** targeting one of our hook addresses (not seen in captured packets but possible)

In all cases, the root cause is the same: **bytes were modified in module memory**.

---

<a name="root-cause"></a>
## 3. Root Cause Analysis

### 3.1 Why JMP Patches Are Detectable

MinHook overwrites the first 5+ bytes of the target function with:
```asm
E9 xx xx xx xx    ; JMP rel32 — redirects to our naked handler
```

Original bytes (e.g., `55 8B EC 56 57` for `push ebp; mov ebp, esp; push esi; push edi`) are replaced. When Warden reads those bytes via `repe movsb` (the stable scan function), the server receives `E9 xx xx xx xx` instead of `55 8B EC 56 57` and flags the mismatch.

Additionally, MinHook allocates small executable memory blocks (16-32 bytes) near the hooked function for trampolines — these create a distinctive fingerprint in the process virtual address space.

### 3.2 Why S-Box Cloning Cannot Replace JMP Hooks

The current spoof flow requires modifying CMSG plaintext **BEFORE** RC4 encryption:

```
RC4 hook intercepts plaintext → SpoofCmsgIfNeeded() modifies results → RC4 encrypts SPOOFED data
```

S-box cloning (warden_rc4.cpp) only works **AFTER** encryption in SendPacket — by then it's too late to modify the plaintext that gets encrypted. The encrypted ciphertext is a stream cipher output — modifying it corrupts the RC4 stream synchronization.

### 3.3 Why Hardware Breakpoints Solve This

Hardware breakpoints use CPU debug registers (DR0-DR3) to trigger exceptions at specific addresses **without modifying any memory bytes**:
- No `E9` JMP patches
- No trampoline allocations
- No page protection changes
- The module's code remains byte-for-byte identical to what the server expects

---

<a name="research-summary"></a>
## 4. Research Summary

### 4.1 Source Classification

Research files are in `wotlk/docs/reference/researches/warden/1/`. They split into two camps with a critical contradiction:

| Source | Threading Model | Self-Integrity | HW Breakpoints | Enforcement |
|--------|----------------|---------------|----------------|-------------|
| **Files 4, 5, 6** (OwnedCore, TC source) | Single-threaded, synchronous | NOT present in WotLK | **SAFE** — not detected | Server-side only |
| Files 1, 8 (speculative) | Worker threads, async | Present, delayed ~4 min | "Medium-High" risk | Client-side via Lua |

**Verdict**: Files 4, 5, 6 are based on actual reverse engineering by known researchers (Cypher, Jadd, FireHack devs) and TrinityCore/AzerothCore source analysis. Files 1 and 8 appear to extrapolate from modern anti-cheat behavior not applicable to WotLK-era Warden.

### 4.2 Key Findings (High Confidence)

From **File 4** (compass_artifact_wf-bc0774d1):
- Warden 3.3.5a is a **server-validated telemetry collector, NOT a client-side enforcer**
- Module is **single-threaded**, runs synchronously on WoW main thread
- Module exports exactly 4 functions: `fpGenerateRC4Keys`, `fpUnload`, `fpPacketHandler`, `fpTick`
- **"Unauthorized software" popup is triggered by SERVER DISCONNECT**, not by module executing Lua
- Module **never directly triggers UI popups or Lua events** — it only sends scan results to server
- **VEH detection: NOT present** — no enumeration of VEH handler list
- **Hardware breakpoint detection: NOT present** — no `GetThreadContext()` calls
- **MinHook trampoline scanning: NOT explicit** — detection is purely byte comparison at scanned addresses
- Known scanned addresses include: `0x00819210` (FrameScript_Execute), `0x00420541` (WS2_32.Send), `0x0048D4A0` (AddChatMessage), `0x00490430` (SendChatMessage)
- **`.data` segment trap at `0x00AC3DAC`**: runtime value differs from on-disk value — catches bypass attempts that redirect reads to on-disk executable
- **Scan function stable signature**: `56 57 FC 8B 54 24 14 8B 74 24 10 8B 44 24 0C 8B CA 8B F8 C1 E9 02 74 02 F3 A5`
- Local self-check on scan function: **some** module builds crash client if first bytes overwritten (tamper-and-kill, NOT detection-and-report), but **not present in all modules**

From **File 5** (compass_artifact_wf-18a0916b):
- **Hardware breakpoints + VEH is the clear winner** — ranked #1 out of 7 techniques
- Uses DR0-DR3 (4 execution breakpoints), DR7 for enable/config
- Exception code: `EXCEPTION_SINGLE_STEP` (`0x80000004`)
- **Resume Flag** (RF, bit 16 of EFLAGS: `context->EFlags |= 0x10000`) suppresses re-trigger for exactly one instruction
- DR6 status bits B0-B3 identify which breakpoint fired
- Debug registers are **per-thread** — must be set via `SetThreadContext` on each thread
- **Warden 3.3.5a does NOT call `GetThreadContext` or `NtGetContextThread`** (multiple researchers confirmed)
- **Login sequence (battle.net.dll) does check** — but one-time, NOT ongoing Warden scans
- Recommended: hook `NtGetContextThread` to zero DR fields as defense-in-depth
- Implementation complexity: ~200-300 lines of focused C code

From **File 6** (compass_artifact_wf-35fa190d):
- Module binary format: 40-byte header, RLE sections, delta-encoded relocs (NOT PE/DLL)
- 7-function callback table passed during Initialize: `fpSendPacket`, `fpCheckModule`, `fpLoadModule`, `fpAllocateMemory`, `fpReleaseMemory`, `fpSetRC4Data`, `fpGetRC4Data`
- MODULE_INITIALIZE packet passes client function pointers: `SFileOpenFile`@0x002485F0, `SFileReadFile`@0x00248460, `SFileCloseFile`@0x00248730, `SFileGetFileSize`@0x002487F0, `FrameScript::GetText`@0x00419D40, `PerformanceCounter`@0x0046AE20
- OwnedCore researcher Cypher: **"Two simple function hooks are all that's needed"**
- Warden not aggressive about self-integrity checking in WotLK era
- Alternative: hook scan function output buffer — catches MEM_CHECK, PAGE_CHECK, self-integrity simultaneously
- Use captured **runtime** bytes (not on-disk) as clean reference, especially for `.data` section

### 4.3 Ranked Technique Comparison (from File 5)

| Rank | Technique | Code Modified | Warden Risk | Performance | Complexity | Verdict |
|------|-----------|:---:|:---:|:---:|:---:|---------|
| **1** | **Hardware breakpoints + VEH** | None | **Low** | Good | Moderate | **Best — proven in production** |
| 2 | Function pointer hook | None (data) | Low | Excellent | Varies | Excellent if pointer found |
| 3 | PAGE_GUARD + VEH | None | Medium | Very poor | Moderate | Only for rare functions |
| 4 | Inline flicker + scan hook | Temporary | Medium-High | Good | High | Works but race condition |
| 5 | INT3 + VEH | 1 byte | High | Good | Low | Fails no-modification req |
| 6 | Shadow page (dual map) | None | High | Good | Extreme | Not feasible without kernel |
| 7 | Emulation + NX page | None | Medium | Catastrophic | Extreme | Research only |

### 4.4 Complementary Approaches (Future Work)

**MODULE_INITIALIZE Callback Hijacking** (from File 3, 6):
- Intercept MODULE_INITIALIZE before module processes it
- Replace disconnect and detection report function pointers with no-ops
- Structural attack — resilient to module code changes
- Works across all modules if protocol standardized
- NOT needed for Phase 1, but good defense-in-depth for future

**Scan Function Output Buffer Hooking** (from File 4, 6):
- Hook the stable `copyBytesForScan` function (`56 57 FC 8B 54 24 14...`)
- After scan executes, check if scanned source address overlaps our patches
- If so, overwrite destination buffer with clean bytes
- Handles MEM_CHECK, PAGE_CHECK, self-integrity simultaneously
- Mutually exclusive with "zero code modification" — this IS an inline hook on the scan function
- Best as fallback strategy if we still need some inline hooks elsewhere

---

<a name="architecture"></a>
## 5. Architecture: Current vs New

### 5.1 Current Architecture (MinHook JMP Patches)

```
Pattern Scanner (ScanForDispReferences)
    → Finds RC4 PRGA functions by 0x100/0x101 displacement references
    → Validates prologue (55 8B EC)
    → Installs up to 4 MinHook patches

MinHook Install:
    MH_CreateHook(target, HookedRC4Naked_N, &g_trampolines[N])
    MH_EnableHook(target)
    → Writes E9 xx xx xx xx at target entry (5+ bytes)
    → Allocates trampoline (16-32 bytes) near target

Hook Execution:
    HookedRC4Naked_N:
        pushad / pushfd          ; save all registers
        mov eax, esp
        push eax
        call RC4DetourHandler    ; C++ handler with full context
        add esp, 4
        popfd / popad
        jmp [g_trampolines[N]]  ; execute original via trampoline

RC4DetourHandler:
    → Auto-detect calling convention (6 variants)
    → Extract dataPtr + dataLen
    → Read plaintext BEFORE RC4 modifies it
    → Call SpoofHashResultIfNeeded() / SpoofCmsgIfNeeded()
    → Write spoofed data back to original buffer
    → Store copy in g_capturedPlaintext (one-shot for SendPacket)
```

**Problem**: 5-byte JMP at target address + trampoline allocation = detectable.

### 5.2 New Architecture (Hardware Breakpoints + VEH)

```
Pattern Scanner (ScanForDispReferences) — UNCHANGED
    → Finds RC4 PRGA functions by 0x100/0x101 displacement references
    → Validates prologue (55 8B EC)
    → Returns up to 4 addresses

HW Breakpoint Install:
    For each target address N (0..3):
        DR[N] = target_address
        DR7 |= (1 << (N*2))          ; enable local breakpoint N
        DR7 &= ~(0xF << (16 + N*4))  ; clear R/W + LEN (= execute, 1-byte)
    Apply to all threads via SetThreadContext(CONTEXT_DEBUG_REGISTERS)

VEH Registration:
    AddVectoredExceptionHandler(1, WardenRC4ExceptionHandler)
    → Priority=1 (first handler in chain)

Exception Handling:
    WardenRC4ExceptionHandler(EXCEPTION_POINTERS* info):
        if (info->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP)
            return EXCEPTION_CONTINUE_SEARCH

        eip = info->ContextRecord->Eip
        Match eip against g_hookedAddrs[0..3]
        if no match → return EXCEPTION_CONTINUE_SEARCH

        // Identify which breakpoint fired via DR6
        dr6 = info->ContextRecord->Dr6
        slot = (dr6 & 1) ? 0 : (dr6 & 2) ? 1 : (dr6 & 4) ? 2 : 3

        // Read function parameters from stack
        esp = info->ContextRecord->Esp
        retAddr = *(DWORD*)(esp)
        stk1 = *(DWORD*)(esp + 4)
        stk2 = *(DWORD*)(esp + 8)
        stk3 = *(DWORD*)(esp + 12)

        // Auto-detect calling convention (same as current)
        // Extract dataPtr + dataLen based on convention
        // Read plaintext, spoof, write back

        // Set Resume Flag to suppress re-trigger
        info->ContextRecord->EFlags |= 0x10000  // RF bit 16

        return EXCEPTION_CONTINUE_EXECUTION
```

**Key differences**:
- No `MH_CreateHook` / `MH_EnableHook` — no bytes modified
- No trampoline allocation — no memory artifacts
- Exception handler replaces naked stub + trampoline JMP
- DR6 identifies which of 4 breakpoints fired
- Resume Flag (RF) replaces trampoline's `jmp back` — lets original instruction execute once without re-triggering

### 5.3 Spoof Flow (Unchanged)

The spoof flow is identical in both architectures. The only difference is HOW we intercept — the WHAT we do with the plaintext is the same:

```
1. Intercept RC4 PRGA call (before it encrypts)
2. Read plaintext buffer from function parameters
3. If HASH_RESULT (0x04): diagnostic log only (DO NOT MODIFY — RC4 re-key desync)
4. If CHEAT_CHECKS_RESULT (0x02):
   a. Pop pending checks from FIFO queue
   b. Walk results by category
   c. MEM_CHECK: replace hook-overlapping data with shadow_copy clean bytes
   d. PAGE_CHECK: force 0xE9 (pass) on hook-overlapping ranges
   e. MODULE/DRIVER/PROC: force 0xE9 (pass)
   f. LUA: replace addon-detection results with empty string
   g. MPQ: replace SHA1 with cached clean hash
   h. Recompute SHA1-XOR-fold checksum
5. Write spoofed plaintext back to buffer
6. Store copy in g_capturedPlaintext (for SendPacket correlation)
7. Original RC4 encrypts the SPOOFED data
```

---

<a name="phase-1"></a>
## 6. Phase 1: Hardware Breakpoints + VEH

### 6.1 Overview

Replace all MinHook usage in `warden_rc4_hook.cpp` with hardware debug registers + VEH. The pattern scanner (`ScanForDispReferences`) remains unchanged — it already produces a list of function addresses.

### 6.2 DR7 Configuration

DR7 bit layout for 4 execution breakpoints:

```
Bit  0: L0 = 1 (local enable DR0)
Bit  1: G0 = 0 (no global)
Bit  2: L1 = 1 (local enable DR1)
Bit  3: G1 = 0
Bit  4: L2 = 1 (local enable DR2)
Bit  5: G2 = 0
Bit  6: L3 = 1 (local enable DR3)
Bit  7: G3 = 0
Bits 8-15: reserved
Bits 16-17: R/W0 = 00 (execute breakpoint)
Bits 18-19: LEN0 = 00 (1-byte, mandatory for execute)
Bits 20-21: R/W1 = 00
Bits 22-23: LEN1 = 00
Bits 24-25: R/W2 = 00
Bits 26-27: LEN2 = 00
Bits 28-29: R/W3 = 00
Bits 30-31: LEN3 = 00
```

For N active breakpoints, set bits L0..L(N-1) = 1, all R/W and LEN = 00.

Example for 3 breakpoints: `DR7 = 0x00000015` (L0=1, L1=1, L2=1).

### 6.3 Thread Enumeration

Debug registers are **per-thread** — must apply to all threads in the process:

```cpp
HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
THREADENTRY32 te = { sizeof(te) };
DWORD pid = GetCurrentProcessId();
DWORD tid = GetCurrentThreadId();

for (Thread32First(snap, &te); ...; Thread32Next(snap, &te)) {
    if (te.th32OwnerProcessID != pid) continue;
    if (te.th32ThreadID == tid) {
        // Current thread: use GetThreadContext/SetThreadContext directly
        // Or: use CONTEXT with GetCurrentThread()
    } else {
        HANDLE hThread = OpenThread(THREAD_ALL_ACCESS, FALSE, te.th32ThreadID);
        SuspendThread(hThread);

        CONTEXT ctx = {};
        ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;
        GetThreadContext(hThread, &ctx);

        ctx.Dr0 = g_hookedAddrs[0];  // (if N >= 1)
        ctx.Dr1 = g_hookedAddrs[1];  // (if N >= 2)
        ctx.Dr2 = g_hookedAddrs[2];  // (if N >= 3)
        ctx.Dr3 = g_hookedAddrs[3];  // (if N >= 4)
        ctx.Dr7 = computeDR7(g_numHooks);

        SetThreadContext(hThread, &ctx);
        ResumeThread(hThread);
        CloseHandle(hThread);
    }
}
CloseHandle(snap);
```

**Important**: Warden 3.3.5a is **single-threaded** (all scan processing on main thread). The RC4 function is called on the main thread. So setting breakpoints on the main thread should be sufficient. However, some modules MAY use a secondary thread for RC4 operations (observed in logs with different TIDs for module thread). Apply to all threads for safety.

**Newly created threads**: Warden modules use `VirtualAlloc` but NOT `CreateThread` in WotLK. However, if needed, we could hook `NtCreateThreadEx` or periodically re-apply breakpoints. For now, applying once during `Install()` should suffice since module doesn't create new threads after initialization.

### 6.4 VEH Handler Implementation

```cpp
LONG CALLBACK WardenRC4ExceptionHandler(EXCEPTION_POINTERS* pExInfo)
{
    // 1. Quick rejection: only handle EXCEPTION_SINGLE_STEP
    if (pExInfo->ExceptionRecord->ExceptionCode != 0x80000004)
        return EXCEPTION_CONTINUE_SEARCH;

    DWORD eip = pExInfo->ContextRecord->Eip;

    // 2. Match against our breakpoint addresses
    int slot = -1;
    for (int i = 0; i < g_numHooks; ++i) {
        if (eip == g_hookedAddrs[i]) {
            slot = i;
            break;
        }
    }
    if (slot < 0)
        return EXCEPTION_CONTINUE_SEARCH;  // Not our breakpoint

    // 3. Read function parameters from stack
    DWORD esp = pExInfo->ContextRecord->Esp;
    DWORD retAddr = *(DWORD*)(esp);
    DWORD stk1 = *(DWORD*)(esp + 4);
    DWORD stk2 = *(DWORD*)(esp + 8);
    DWORD stk3 = *(DWORD*)(esp + 12);

    // 4. Also read all registers for convention detection
    DWORD eax = pExInfo->ContextRecord->Eax;
    DWORD ecx = pExInfo->ContextRecord->Ecx;
    DWORD edx = pExInfo->ContextRecord->Edx;
    DWORD ebx = pExInfo->ContextRecord->Ebx;
    DWORD esi = pExInfo->ContextRecord->Esi;
    DWORD edi = pExInfo->ContextRecord->Edi;

    // 5. Call RC4DetourHandler logic (same as current implementation)
    //    - Auto-detect calling convention
    //    - Extract dataPtr + dataLen
    //    - Read plaintext
    //    - Spoof if needed
    //    - Write back
    //    - Capture in g_capturedPlaintext
    HandleRC4Intercept(eax, ecx, edx, ebx, esi, edi, stk1, stk2, stk3);

    // 6. Set Resume Flag to suppress re-trigger for one instruction
    pExInfo->ContextRecord->EFlags |= 0x10000;

    // 7. Clear DR6 status bits (acknowledge breakpoint)
    pExInfo->ContextRecord->Dr6 = 0;

    return EXCEPTION_CONTINUE_EXECUTION;
}
```

**Critical details**:
- **Resume Flag (RF, bit 16 of EFLAGS)**: suppresses debug fault for exactly one instruction. After the original first instruction executes, RF is automatically cleared by the CPU. This lets the function execute normally without re-triggering the breakpoint.
- **DR6 clearing**: DR6 holds status bits indicating which breakpoints fired. Must be cleared to avoid confusion on next exception.
- **SEH safety**: the VEH handler MUST NOT use C++ objects (std::string, etc.) — use plain C types only. Same constraint as current `__declspec(naked)` handlers.
- **Exception code**: `EXCEPTION_SINGLE_STEP` is `0x80000004`. Execution breakpoints fire BEFORE the instruction executes, so EIP points directly at the breakpoint address. This is different from single-step (Trap Flag), which fires AFTER.

### 6.5 Calling Convention Detection

The current RC4DetourHandler supports 6 calling conventions. This logic is unchanged — the VEH handler simply extracts the same register/stack values and passes them to the same detection logic.

Conventions observed across modules:
1. `__thiscall`: ECX=ctx, stk1=data, stk2=len
2. `__cdecl`: stk1=ctx, stk2=data, stk3=len
3. ECX-variant: ECX=ctx, stk1=len, stk2=data
4. EDX-variant: EDX=ctx, stk1=data, stk2=len
5. **EAX-variant**: EAX=ctx, stk1=data, stk2=len (observed in modules 0BE6B21C, 2E9FE85D)
6. EAX-variant2: EAX=ctx, stk1=len, stk2=data

Convention is detected by checking `LooksLikeRC4Context(ptr)` — validates that ptr+0x100 and ptr+0x101 contain valid i/j values and S-box at ptr is a valid permutation of 0-255.

### 6.6 Key Difference: No Trampoline

With MinHook, after the handler runs, execution jumps to the trampoline which contains the original overwritten instructions + a JMP back to original+5. With hardware breakpoints, there is NO trampoline:

1. Exception fires BEFORE first instruction
2. VEH handler runs (reads params, spoofs, captures)
3. VEH handler sets Resume Flag
4. Returns `EXCEPTION_CONTINUE_EXECUTION`
5. CPU resumes at EIP (= breakpoint address)
6. RF suppresses breakpoint for this one instruction
7. First instruction executes normally
8. RF clears automatically
9. Function continues normally until next call triggers breakpoint again

This means the original function executes **completely unmodified** — perfect byte-for-byte integrity.

### 6.7 Changes to warden_rc4_hook.cpp

**Remove**:
- All `#include <MinHook.h>` references
- `g_trampolines[]` array
- `HookedRC4Naked_0` through `HookedRC4Naked_3` (naked stubs)
- `MH_CreateHook()` / `MH_EnableHook()` / `MH_DisableHook()` / `MH_RemoveHook()` calls

**Add**:
- `#include <TlHelp32.h>` for thread enumeration
- `static PVOID g_vehHandle = nullptr;` — VEH registration handle
- `LONG CALLBACK WardenRC4ExceptionHandler(EXCEPTION_POINTERS*)` — VEH handler
- `SetHardwareBreakpoints(bool enable)` — applies/clears DR0-DR3 on all threads
- `HandleRC4Intercept(registers...)` — extracted from RC4DetourHandler, called by VEH

**Preserve unchanged**:
- `ScanForDispReferences()` — pattern scanner
- Convention detection logic
- `g_capturedPlaintext[]` + `g_capturedLen` + `g_capturedValid`
- `g_hookedAddrs[]` + `g_numHooks`
- CRITICAL_SECTION for thread safety
- `ConsumePlaintext()` API
- `ValidateDecryptedCmsg()` validation
- Speculative CMSG scan fallback

### 6.8 Changes to hooks.cpp

**Minimal changes** — the integration points remain the same:

```cpp
// In WardenPostHandler, after HASH_REQUEST:
warden_rc4_hook::Install(moduleBase, moduleSize);  // Now uses HW breakpoints

// In MODULE_USE:
warden_rc4_hook::Remove();  // Now clears DR0-DR3

// In SendPacketHandler:
warden_rc4_hook::ConsumePlaintext(buf, size, &len);  // Unchanged API
```

The only difference: `MH_Initialize()` / `MH_Uninitialize()` calls in `Initialize()` / `Shutdown()` still needed for other hooks (FrameScript_Execute, SMSG_WARDEN_DATA, SendPacket, ARC4::Process). Only the RC4 hooks move to hardware breakpoints.

### 6.9 Potential Issues and Mitigations

**Issue 1: DR registers consumed by debugger**
If a debugger is attached and uses DR0-DR3, our breakpoints will conflict.
**Mitigation**: Check if DR registers are already in use before setting. Log warning if conflict detected. In production (no debugger), this is not an issue.

**Issue 2: VEH handler performance**
Every `EXCEPTION_SINGLE_STEP` in the process hits our handler first.
**Mitigation**: Quick rejection (check exception code, then check EIP against 4 addresses). This is O(1) per exception — negligible overhead. RC4 is called a few times per Warden cycle (~10-60 second intervals), not on every frame.

**Issue 3: Thread safety of VEH handler**
The VEH handler can be called from any thread. The current RC4DetourHandler already uses CRITICAL_SECTION for g_capturedPlaintext access.
**Mitigation**: Same CRITICAL_SECTION usage. The VEH handler itself doesn't need additional synchronization — CONTEXT is per-exception-delivery.

**Issue 4: MinHook MH_ERROR_MEMORY_PROTECT on cleanup**
Currently when old module memory is freed (VirtualFree'd by the client), MH_DisableHook fails because memory protection can't be changed on freed memory.
**Mitigation**: **Eliminated entirely** — no MinHook hooks to remove. Just clear DR registers on `Remove()`.

---

<a name="phase-2"></a>
## 7. Phase 2: KSA False Positive Filtering

### 7.1 Problem

The current pattern scanner (`ScanForDispReferences`) looks for instructions referencing displacement 0x100 and 0x101, which are RC4-related constants. However, both PRGA (encryption/decryption) and KSA (key setup) use these displacements:

- **PRGA**: `S[i]` where i increments via `S[(i+1) & 0xFF]` — uses offset 0x100 for i counter
- **KSA**: `S[i]` initialization loop also uses these offsets

Hook #2 at module+0x293a was identified as KSA because:
- It was called with len=0 (no-op PRGA would return immediately, but KSA has different semantics)
- Or it performed an identity initialization loop (setting S[i] = i for i=0..255)

KSA hooks are **useless** (they don't process plaintext) and **increase detection surface** (extra modified bytes for no benefit).

### 7.2 Solution

Add KSA detection heuristic to the scanner:

1. **After finding prologue**, scan forward for KSA signature patterns:
   - Identity loop: `mov [reg+offset], reg` where offset increments 0..255
   - Key mixing: XOR/ADD operations with key material
   - Absence of data pointer parameter (KSA takes key+keyLen, not data+dataLen)

2. **Call-based detection** (current approach, keep as fallback):
   - If first call has len=0 or unusual register state → likely KSA
   - Mark as KSA and skip

3. **Prologue comparison**: KSA often has different register usage patterns:
   - KSA: typically uses ESI for counter, expects key pointer
   - PRGA: uses ECX/EAX for context, expects data+len

### 7.3 Implementation

Add to `ScanForDispReferences()`:

```cpp
// After finding cluster prologue at 'funcAddr':
if (LooksLikeKSA(funcAddr, moduleBase, moduleSize)) {
    LOG(INFO) << "Skipping KSA at " << hex << funcAddr;
    continue;  // Don't add to hookable list
}
```

Where `LooksLikeKSA()` scans the first ~50 bytes for identity init patterns (`S[i] = i` loop) or key-schedule characteristics.

---

<a name="phase-3"></a>
## 8. Phase 3: Defensive NtGetContextThread Hook

### 8.1 Rationale

While Warden 3.3.5a does NOT call `GetThreadContext` to read debug registers, this defense-in-depth measure protects against:
- Future module updates that add DR inspection
- Private server custom modules with enhanced detection
- Any third-party anti-cheat running alongside

### 8.2 Implementation

Hook `NtGetContextThread` (ntdll.dll) via MinHook (this is a standard API hook, NOT inside the Warden module, so byte modification is irrelevant):

```cpp
typedef NTSTATUS (NTAPI* NtGetContextThread_t)(HANDLE, PCONTEXT);
static NtGetContextThread_t g_origNtGetContextThread = nullptr;

NTSTATUS NTAPI HookedNtGetContextThread(HANDLE hThread, PCONTEXT ctx)
{
    NTSTATUS status = g_origNtGetContextThread(hThread, ctx);

    if (NT_SUCCESS(status) && ctx &&
        (ctx->ContextFlags & CONTEXT_DEBUG_REGISTERS))
    {
        ctx->Dr0 = 0;
        ctx->Dr1 = 0;
        ctx->Dr2 = 0;
        ctx->Dr3 = 0;
        ctx->Dr6 = 0;
        ctx->Dr7 = 0;
    }

    return status;
}
```

Install this hook in `hooks::Initialize()` alongside other MinHook-based hooks. This is safe because:
- `NtGetContextThread` is in ntdll.dll (system DLL), not in Warden module
- Warden MEM_CHECK targets are in WoW.exe address range, not ntdll.dll
- Even if Warden checked ntdll bytes (unlikely), we could spoof those too

### 8.3 Also Hook GetThreadContext

`GetThreadContext` (kernel32.dll) is the user-mode wrapper around `NtGetContextThread`. Some callers may use it directly:

```cpp
typedef BOOL (WINAPI* GetThreadContext_t)(HANDLE, LPCONTEXT);
static GetThreadContext_t g_origGetThreadContext = nullptr;

BOOL WINAPI HookedGetThreadContext(HANDLE hThread, LPCONTEXT ctx)
{
    BOOL result = g_origGetThreadContext(hThread, ctx);

    if (result && ctx && (ctx->ContextFlags & CONTEXT_DEBUG_REGISTERS))
    {
        ctx->Dr0 = 0;
        ctx->Dr1 = 0;
        ctx->Dr2 = 0;
        ctx->Dr3 = 0;
        ctx->Dr6 = 0;
        ctx->Dr7 = 0;
    }

    return result;
}
```

---

<a name="phase-4"></a>
## 9. Phase 4: Scan Function Output Buffer Hooking (Optional)

### 9.1 Rationale

This is an alternative/complementary approach. Instead of (or in addition to) spoofing at the RC4 level, we can hook the scan function itself to return clean bytes for any address that overlaps our modifications.

This is only needed if:
- We have OTHER inline hooks besides RC4 (FrameScript_Execute, SMSG_WARDEN_DATA, SendPacket, ARC4::Process are all MinHook patches)
- We want to hide those hooks from Warden scans too

Currently, the spoof logic in `warden_spoof.cpp` already handles MEM_CHECK/PAGE_CHECK by replacing results with clean bytes. But that relies on the RC4 hook being active to intercept the CMSG. The scan function hook would be an independent layer.

### 9.2 Stable Signature

Pattern to find: `56 57 FC 8B 54 24 14 8B 74 24 10 8B 44 24 0C 8B CA 8B F8 C1 E9 02 74 02 F3 A5`

This is a memcpy-like function used by ALL Warden modules to read client memory:
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

### 9.3 Implementation (if needed)

This would use a hardware breakpoint on the `ret` instruction (at pattern end: `5F 5E C3`), or on the function entry. After the function executes (copies real bytes), check if `ESI` (source address) overlaps any of our hook addresses. If so, overwrite the destination buffer with original bytes.

**Caution**: This approach requires hooking inside the Warden module too. If using a JMP patch, it has the same problem as our RC4 hooks. If using a hardware breakpoint, it consumes one of our 4 DR registers (leaving only 3 for RC4).

**Recommendation**: Defer this phase. The hardware breakpoint approach for RC4 hooks eliminates the primary detection vector. Our other hooks (FrameScript_Execute etc.) are in WoW.exe, and we already spoof their MEM_CHECK results via `warden_spoof.cpp`.

---

<a name="technical-reference"></a>
## 10. Technical Reference

### 10.1 x86 Debug Register Layout

```
DR0: Linear address for breakpoint 0
DR1: Linear address for breakpoint 1
DR2: Linear address for breakpoint 2
DR3: Linear address for breakpoint 3
DR4: Reserved (alias for DR6)
DR5: Reserved (alias for DR7)
DR6: Debug Status Register
     Bit 0 (B0): Breakpoint 0 was triggered
     Bit 1 (B1): Breakpoint 1 was triggered
     Bit 2 (B2): Breakpoint 2 was triggered
     Bit 3 (B3): Breakpoint 3 was triggered
     Bit 14 (BD): Debug register access detected
     Bit 15 (BS): Single-step (Trap Flag)
DR7: Debug Control Register
     Bit 0 (L0): Local enable BP 0
     Bit 1 (G0): Global enable BP 0
     Bit 2 (L1): Local enable BP 1
     Bit 3 (G1): Global enable BP 1
     ...
     Bits 16-17: R/W0 (00=execute, 01=write, 11=read/write)
     Bits 18-19: LEN0 (00=1-byte [required for execute], 01=2-byte, 11=4-byte)
     Bits 20-21: R/W1
     Bits 22-23: LEN1
     Bits 24-25: R/W2
     Bits 26-27: LEN2
     Bits 28-29: R/W3
     Bits 30-31: LEN3
```

### 10.2 EXCEPTION_SINGLE_STEP vs EXCEPTION_BREAKPOINT

- `EXCEPTION_SINGLE_STEP` (`0x80000004`): Fired by **hardware breakpoints** (DR0-DR3 execution breaks) and by **Trap Flag** (TF in EFLAGS). This is a **fault** for execution breakpoints — EIP points at the target address BEFORE it executes.
- `EXCEPTION_BREAKPOINT` (`0x80000003`): Fired by `INT 3` (`0xCC`) software breakpoint. Kernel adjusts EIP back by 1 before delivery.

Our VEH handler must check for `0x80000004` only.

### 10.3 Resume Flag (RF)

- Bit 16 of EFLAGS: `context->EFlags |= 0x10000`
- When set, suppresses debug fault for exactly one instruction
- CPU automatically clears RF after the instruction executes
- This is how we prevent the breakpoint from firing in an infinite loop:
  1. Breakpoint fires → VEH handler runs
  2. Handler sets RF → returns EXCEPTION_CONTINUE_EXECUTION
  3. CPU resumes at EIP, RF suppresses breakpoint
  4. First instruction executes normally
  5. RF clears, breakpoint active again for next call

### 10.4 Warden Module Memory Characteristics

- Allocated via `VirtualAlloc(NULL, moduleSize, MEM_COMMIT, PAGE_EXECUTE_READWRITE)`
- `MEM_PRIVATE` allocation type (confirmed — NOT `MEM_IMAGE`)
- Size: 45,056 to 49,152 bytes (depends on module)
- Contains: unpacked code sections, data sections, import table, export table
- RWX permissions required for dynamic unpacking + relocation + execution
- Our hooks target RC4 PRGA functions at various offsets (module-specific)

### 10.5 RC4 Context Memory Layout

All Warden modules use the same RC4 context structure:
```
[S[256]][i:1][j:1]    — 258 bytes total
```
Where:
- `S[256]` is the substitution box (permutation of 0-255) at offset +0x000
- `i` (PRGA counter) at offset +0x100 (1 byte)
- `j` (PRGA counter) at offset +0x101 (1 byte)

This layout is what the pattern scanner looks for (0x100 and 0x101 displacements in ModRM bytes).

### 10.6 Warden Module Callback Table

Passed during Initialize (ordinal 1, `__fastcall`, ECX=pointer to table):

```c
struct FuncList {                    // 28 bytes (7 x DWORD)
    0x00: fpSendPacket               // Send warden response to server
    0x04: fpCheckModule              // Check if sub-module is cached
    0x08: fpLoadModule               // Load a sub-module
    0x0C: fpAllocateMemory           // Heap alloc
    0x10: fpReleaseMemory            // Heap free
    0x14: fpSetRC4Data               // Persist RC4 state
    0x18: fpGetRC4Data               // Retrieve RC4 state
};
```

Module returns pointer-to-pointer to export table:
```c
struct WardenFuncList {              // 16 bytes (4 x DWORD)
    0x00: fpGenerateRC4Keys          // Process seed → new RC4 keys
    0x04: fpUnload                   // Teardown
    0x08: fpPacketHandler            // Main scan request processor
    0x0C: fpTick                     // Periodic tick (timing delta)
};
```

### 10.7 MODULE_INITIALIZE Function Pointers (3.3.5a build 12340)

Passed via `WARDEN_SMSG_MODULE_INITIALIZE` (sub-opcode 3, 57-byte struct):

| Address | Function | Purpose |
|---------|----------|---------|
| `0x002485F0` | SFileOpenFile | MPQ archive access |
| `0x002487F0` | SFileGetFileSize | MPQ file size |
| `0x00248460` | SFileReadFile | MPQ file read |
| `0x00248730` | SFileCloseFile | MPQ file close |
| `0x00419D40` | FrameScript::GetText | Lua variable reading |
| `0x0046AE20` | PerformanceCounter | Timing checks |

### 10.8 Known Scanned Addresses (build 12340)

These addresses are targeted by MEM_CHECK. Any byte modification at these addresses will be detected:

| Address | Len | Target | Category |
|---------|-----|--------|----------|
| `0x00819210` | 1+ | FrameScript_Execute | Lua unlocker |
| `0x0084F860` | — | FrameScript_Load | Lua unlocker |
| `0x005120E0` | — | LoadScriptFunctions | Lua unlocker |
| `0x0049DBB2` | 7 | Protected Lua Func Check | Lua protection |
| `0x004AA9C2` | 5 | FrameXML Signature Check | UI integrity |
| `0x008C8398` | 8 | Max Wall Climb angle | Physics hack |
| `0x008C845C` | 8 | Gravity constant | Physics hack |
| `0x008F7AC8` | 8 | Jump Velocity | Physics hack |
| `0x005CDC20` | 6 | Falling Check | Fly hack |
| `0x00420541` | 10 | WS2_32.Send | Network hook |
| `0x0048D4A0` | 12 | AddChatMessage | Chat hook |
| `0x00490430` | 12 | SendChatMessage | Chat hook |

**Our hooks at these addresses**: FrameScript_Execute (`0x00819210`) is hooked via MinHook. The spoof system in `warden_spoof.cpp` replaces MEM_CHECK results for overlapping addresses with clean bytes from `shadow_copy`.

### 10.9 CMSG_WARDEN_DATA Formats

```
MODULE_MISSING:        [0x00]                    = 1 byte
MODULE_OK:             [0x01]                    = 1 byte
CHEAT_CHECKS_RESULT:   [0x02][resultLen:2LE][checksum:4][results:N] = 7+N bytes
HASH_RESULT:           [0x04][SHA1:20]           = 21 bytes
MODULE_FAILED:         [0x05]                    = 1 byte
```

Checksum algorithm: `SHA1(results) → 5 x uint32_t LE → XOR-fold → uint32_t`.

### 10.10 Per-Check Result Formats in CHEAT_CHECKS_RESULT

| Category | Success | Failure | Size |
|----------|---------|---------|------|
| TIMING | `[result:1][ticks:4]` | same | Always 5 bytes |
| MEM | `[0x00][data:readLen]` | `[nonzero:1]` | 1+readLen or 1 |
| PAGE | `[0xE9]` (hash match) | `[other:1]` | Always 1 byte |
| PROC | `[0xE9]` (HMAC match) | `[other:1]` | Always 1 byte |
| MODULE | `[0xE9]` (not found = pass) | `[other:1]` | Always 1 byte |
| DRIVER | `[0xE9]` (not found = pass) | `[other:1]` | Always 1 byte |
| MPQ | `[0x00][SHA1:20]` | `[nonzero:1]` | 21 or 1 |
| LUA | `[0x00][strlen:1][string:N]` | `[nonzero:1]` | 2+N or 1 |

---

<a name="files-inventory"></a>
## 11. Files Inventory

### 11.1 Files to Modify

| File | Changes |
|------|---------|
| `wotlk/src/warden/warden_rc4_hook.h` | Update public API if needed (likely unchanged) |
| `wotlk/src/warden/warden_rc4_hook.cpp` | **Major rewrite**: replace MinHook with DR+VEH |
| `wotlk/src/hooks/hooks.cpp` | Minor: ensure MH_Initialize still called for other hooks |

### 11.2 Files Unchanged

| File | Reason |
|------|--------|
| `wotlk/src/warden/warden_rc4.h/.cpp` | S-box cloning fallback — keep as-is |
| `wotlk/src/warden/warden_spoof.h/.cpp` | Spoof logic — completely unchanged |
| `wotlk/src/warden/warden_scan.h/.cpp` | Type ID discovery — unchanged |
| `wotlk/src/warden/warden_types.h` | Type definitions — unchanged |
| `wotlk/src/warden/warden_checksum.h/.cpp` | SHA1 checksum — unchanged |

### 11.3 Files to Add (Phase 3 only)

| File | Purpose |
|------|---------|
| (none — NtGetContextThread hook goes into hooks.cpp) | |

### 11.4 Research References

All research files in `wotlk/docs/reference/researches/warden/1/`:
1. `Decoding the FC6560DA Warden...md` — Multi-layered defense analysis (speculative)
2. `Evading Warden...md` — 7 stealth hooking techniques compared
3. `Universal Warden Bypass...md` — MODULE_INITIALIZE callback hijacking
4. `compass_artifact_wf-bc0774d1...md` — **KEY**: Warden internals, server-validated, single-threaded
5. `compass_artifact_wf-18a0916b...md` — **KEY**: HW breakpoints winner, DR7 layout, implementation details
6. `compass_artifact_wf-35fa190d...md` — **KEY**: Module format, callback table, scan function signature
7. `Neutralizing Warden Anti-Cheat Detection.md` — 6 neutralization vectors
8. `Warden Module Hook Detection Analysis.md` — Advanced detection heuristics (speculative)

---

<a name="implementation-order"></a>
## 12. Implementation Order

### Step 1: Extract RC4DetourHandler logic into HandleRC4Intercept()
- Separate the convention detection + spoof + capture logic from the naked stub mechanics
- New function signature: `void HandleRC4Intercept(DWORD eax, ecx, edx, ebx, esi, edi, stk1, stk2, stk3)`
- This function is called from both the current naked stubs AND the new VEH handler
- Test: existing MinHook-based hooks still work with refactored code

### Step 2: Implement VEH handler
- Register via `AddVectoredExceptionHandler(1, WardenRC4ExceptionHandler)`
- Handle `EXCEPTION_SINGLE_STEP` with EIP matching
- Extract registers from CONTEXT, call `HandleRC4Intercept()`
- Set Resume Flag, clear DR6
- Test: handler compiles and handles exceptions correctly (can test with manually-set breakpoint)

### Step 3: Implement SetHardwareBreakpoints()
- Thread enumeration via CreateToolhelp32Snapshot
- DR0-DR3 and DR7 configuration
- Support both install (set addresses) and remove (clear all)
- Test: breakpoints trigger VEH handler on known addresses

### Step 4: Replace MinHook calls in Install() / Remove()
- `Install()`: scan patterns → SetHardwareBreakpoints(true) instead of MH_CreateHook
- `Remove()`: SetHardwareBreakpoints(false) instead of MH_DisableHook
- Remove naked stubs and trampoline references
- Test: full end-to-end with Warden module

### Step 5: Add KSA filtering (Phase 2)
- Implement `LooksLikeKSA()` heuristic in scanner
- Skip KSA functions from hookable list
- Test: module FC6560DA correctly hooks only 2 PRGA functions instead of 3

### Step 6: Add NtGetContextThread hook (Phase 3)
- MinHook-based hook in hooks.cpp Initialize()
- Zero DR fields in returned CONTEXT
- Test: calling GetThreadContext returns zeroed debug registers

---

<a name="acceptance-criteria"></a>
## 13. Acceptance Criteria

### Phase 1 (Hardware Breakpoints + VEH)
- [ ] No MinHook patches inside Warden module memory
- [ ] No trampoline allocations near Warden module
- [ ] VEH handler correctly intercepts RC4 PRGA calls
- [ ] Calling convention auto-detection works (all 6 variants)
- [ ] Plaintext captured correctly (CMSG structural validation passes)
- [ ] SpoofCmsgIfNeeded() modifies results before encryption
- [ ] SpoofHashResultIfNeeded() logs but does NOT modify HASH_RESULT
- [ ] ConsumePlaintext() returns valid data for SendPacket correlation
- [ ] Deferred install timing works (after HASH_REQUEST, before first scan)
- [ ] Remove() clears breakpoints cleanly (no MH_ERROR_MEMORY_PROTECT)
- [ ] Multiple modules in same session work (old module remove → new module install)
- [ ] No crashes during normal gameplay
- [ ] No disconnects from Warden detection in extended session (>30 minutes)

### Phase 2 (KSA Filtering)
- [ ] KSA functions detected and skipped by scanner
- [ ] Only PRGA functions are hooked
- [ ] Reduced detection surface (fewer breakpoints consumed)

### Phase 3 (Defensive NtGetContextThread)
- [ ] GetThreadContext returns zeroed DR0-DR7 for our process threads
- [ ] Normal program operation unaffected (other GetThreadContext callers work)
- [ ] Login-time anti-debug checks pass (battle.net.dll)
