# Stealth RC4 Hooks (Hardware Breakpoints + VEH)

**Date**: 2026-03-03

---

## Summary

Replaced MinHook inline JMP patches inside the Warden module with hardware breakpoints (DR0-DR3) + Vectored Exception Handler (VEH). Zero bytes are modified in Warden module memory, eliminating detection by MEM_CHECK, PAGE_CHECK, and self-integrity verification. Added KSA filtering (Phase 2) and NtGetContextThread defense-in-depth hook (Phase 3).

## Background

During a session on 2026-03-02, the client was disconnected with "Unauthorized software or modifications detected" after ~4 minutes. Analysis showed that MinHook's 5-byte `E9` JMP patches at 3 locations inside the Warden module were the likely cause — detected by PAGE_CHECK hash mismatch or future MEM_CHECK targeting hook addresses. All CMSG checksums were valid, so the detection was purely byte-level.

## Changes

### 1. Hardware Breakpoints + VEH (Phase 1) — warden_rc4_hook.cpp

**Major rewrite**: replaced all MinHook usage with CPU debug registers + VEH.

**Removed**:
- `#include <MinHook.h>` — replaced with `#include <TlHelp32.h>`
- `g_trampolines[]` array — no trampolines needed
- `HookedRC4Naked_0` through `HookedRC4Naked_3` — 4 naked assembly stubs
- `g_nakedStubs[]` function pointer array
- `g_hooksActive[]` — no enable/disable toggle per slot
- All `MH_CreateHook` / `MH_EnableHook` / `MH_DisableHook` / `MH_RemoveHook` calls

**Added**:
- `WardenRC4ExceptionHandler()` — VEH handler that catches `EXCEPTION_SINGLE_STEP`, matches EIP against breakpoint addresses, reads registers + stack from `CONTEXT`, calls `HandleRC4Intercept()`
- `HandleRC4Intercept()` — extracted core RC4 hook logic (convention detection, plaintext capture, spoof) from the old `RC4DetourHandler()`, now takes register values directly as parameters instead of reading from pushad/pushfd stack
- `SetHardwareBreakpoints(enable)` — enumerates all threads via `CreateToolhelp32Snapshot`, sets/clears DR0-DR3 + DR7 on each thread
- `ComputeDR7(numBreakpoints)` — computes DR7 value with local enable bits L0-L3, R/W=00 (execute), LEN=00 (1 byte)
- VEH registration via `AddVectoredExceptionHandler(1, ...)` — priority 1 (first in chain), stays registered across module changes
- Resume Flag (`EFlags |= 0x10000`) — suppresses breakpoint for one instruction, letting the original function execute unmodified

**Install flow**:
```
ScanRuntimeForAllRC4() → up to 4 function addresses
  → Store in g_hookedAddrs[]
  → Register VEH (once, persistent)
  → EnableContextGuard() (NtGetContextThread hook)
  → SetHardwareBreakpoints(true) on all threads
```

**Remove flow**:
```
SetHardwareBreakpoints(false) → clear all DR registers
  → DisableContextGuard()
  → Zero g_hookedAddrs[]
```

**Cleanup flow**:
```
RemoveVectoredExceptionHandler() → unregister VEH
  → DeleteCriticalSection()
```

**Preserved unchanged**: pattern scanner, convention detection (6 variants), speculative CMSG scan, `ConsumePlaintext()` API, CRITICAL_SECTION thread safety, structural CMSG validation.

### 2. KSA False Positive Filtering (Phase 2) — warden_rc4_hook.cpp

**Added**: `LooksLikeKSA()` — heuristic that scans the first 80 bytes of a function for CMP/SUB instructions with immediate value `0x100`, which is the identity initialization loop bound (`for i = 0..255: S[i] = i`). PRGA uses 0x100/0x101 only as memory displacements, never as immediate operands.

**Detection patterns**:
- `3D 00 01 00 00` — `CMP EAX, 0x100`
- `81 [C0-FF] 00 01 00 00` — Group 1 `CMP/SUB/etc reg, 0x100` with mod=11 (register direct)

**Impact**: KSA functions no longer consume a precious hardware breakpoint slot (max 4). In the FC6560DA module incident, hook #2 at module+0x293a was KSA — useless (doesn't process plaintext) and extra detection surface.

### 3. NtGetContextThread Defense-in-Depth (Phase 3) — hooks.cpp, hooks.h

**Added to hooks.h**: `EnableContextGuard()` / `DisableContextGuard()` — toggle the NtGetContextThread hook on demand.

**Added to hooks.cpp**:
- `HookedNtGetContextThread()` — MinHook on `ntdll!NtGetContextThread`, zeroes DR0-DR7 fields in returned CONTEXT when the debug registers flag (`0x10`) is set. NtGetContextThread alone is sufficient — all `GetThreadContext` calls go through it internally.
- Hook is **created but disabled** in `Initialize()` — avoids interference with login/authentication phase (battle.net.dll checks debug registers once during login).
- **Enabled lazily** by `EnableContextGuard()` only when hardware breakpoints are set.
- **Disabled** by `DisableContextGuard()` when breakpoints are cleared.
- Properly disabled in `Shutdown()`.

**CONTEXT_DEBUG_REGISTERS flag check**: uses `ctx->ContextFlags & 0x10` instead of `CONTEXT_DEBUG_REGISTERS` directly, because `CONTEXT_DEBUG_REGISTERS = CONTEXT_i386 | 0x10` and `CONTEXT_i386` (0x10000) is always set in valid x86 contexts.

### 4. Task Documentation Update

**File modified**: `wotlk/docs/tasks/TASK_009_warden-stealth-hooks.md`

Status changed from `TODO` to `DONE`.

## Technical Details

### Why Hardware Breakpoints Are Undetectable

Hardware breakpoints use CPU debug registers — no memory modification occurs:
- No `E9 xx xx xx xx` JMP at function entry
- No trampoline allocation near the module
- No page protection changes (`VirtualProtect`)
- Module code remains byte-for-byte identical to server's copy

### How VEH Intercept Works

1. CPU hits execution breakpoint → `EXCEPTION_SINGLE_STEP` before instruction executes
2. VEH handler matches EIP against stored addresses
3. Reads all registers + stack args from `CONTEXT`
4. Calls `HandleRC4Intercept()` — same spoof/capture logic as before
5. Sets Resume Flag (`RF`, bit 16 of EFLAGS) — suppresses breakpoint for one instruction
6. Returns `EXCEPTION_CONTINUE_EXECUTION` — original instruction executes normally
7. RF clears automatically after one instruction → breakpoint active for next call

### Thread Coverage

`SetHardwareBreakpoints()` enumerates all process threads:
- Current thread: uses pseudo-handle (no suspend needed)
- Other threads: `OpenThread` → `SuspendThread` → `SetThreadContext` → `ResumeThread`

Warden's RC4 may be called from both main thread and module thread, so all threads are covered.

## Files Changed

| File | Action |
|------|--------|
| `src/warden/warden_rc4_hook.cpp` | Major rewrite (MinHook → HW breakpoints + VEH) |
| `src/hooks/hooks.cpp` | Added NtGetContextThread hook (create/enable/disable/shutdown) |
| `src/hooks/hooks.h` | Added `EnableContextGuard()` / `DisableContextGuard()` |
| `docs/tasks/TASK_009_warden-stealth-hooks.md` | Status: TODO → DONE |
