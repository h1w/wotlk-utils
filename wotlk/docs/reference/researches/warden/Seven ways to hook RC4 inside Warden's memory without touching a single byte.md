# Seven ways to hook RC4 inside Warden's memory without touching a single byte

**Hardware breakpoint hooking via debug registers (DR0–DR3) paired with a Vectored Exception Handler is the clear winner for intercepting up to 4 functions inside Warden's ~48KB VirtualAlloc'd code block in WoW 3.3.5a.** This approach modifies zero code bytes, making it invisible to Warden's MEM_CHECK integrity scans — which read raw bytes at specific addresses and send them to the server for comparison. In the 3.3.5a era, Warden does not scan debug registers, does not walk the VEH handler chain, and does not integrity-check its own dynamically loaded module code. The remaining six techniques each carry meaningful trade-offs in detectability, performance, or feasibility that make them inferior for this specific scenario.

The analysis below evaluates each approach against four criteria: Warden detectability, usermode feasibility, pre-execution data modification capability, and implementation complexity. The target is a dynamically allocated `PAGE_EXECUTE_READWRITE` region containing Warden's own downloaded scanning module, where an RC4 encryption function must be intercepted to read and modify a data buffer before encryption proceeds.

---

## 1. Hardware breakpoints deliver stealth through CPU registers, not memory patches

Hardware breakpoints use the x86 debug registers DR0–DR3 to hold linear addresses and DR7 to configure breakpoint conditions. For execution breakpoints, DR7's R/W field is set to `00` (instruction execution only) and the LEN field to `00` (1-byte, mandatory for execute breakpoints). Each register pair enables one breakpoint, giving exactly **4 simultaneous hooks** — matching the requirement precisely.

**Exception mechanics matter deeply here.** Execution breakpoints are *faults*, not traps. The exception fires *before* the instruction at the breakpoint address executes, and EIP in the delivered `CONTEXT` structure points directly to the breakpointed address. The exception code is `EXCEPTION_SINGLE_STEP` (`0x80000004`), which the VEH handler must filter. DR6 status bits B0–B3 identify which breakpoint fired. The handler reads function parameters directly from the stack via `CONTEXT->Esp` — for a `__cdecl` or `__stdcall` RC4 function, `[ESP+4]` holds the first parameter, `[ESP+8]` the buffer pointer, `[ESP+12]` the length. After modifying the buffer, the handler sets the **Resume Flag** (RF, bit 16 of EFLAGS: `context->EFlags |= 0x10000`) and returns `EXCEPTION_CONTINUE_EXECUTION`. RF suppresses the debug fault for exactly one instruction, letting the original function execute normally without re-triggering.

The DR7 bit layout for enabling all four breakpoints simultaneously:

| Bits | Field | Value | Purpose |
|------|-------|-------|---------|
| 0 | L0 | 1 | Local enable DR0 |
| 2 | L1 | 1 | Local enable DR1 |
| 4 | L2 | 1 | Local enable DR2 |
| 6 | L3 | 1 | Local enable DR3 |
| 16–17 | R/W0 | 00 | Execute breakpoint |
| 18–19 | LEN0 | 00 | 1-byte (required) |
| 20–21 | R/W1 | 00 | Execute breakpoint |
| ... | ... | ... | Same pattern for DR1–DR3 |

**Debug registers are per-thread state.** You must call `SuspendThread` → `GetThreadContext(CONTEXT_DEBUG_REGISTERS)` → modify DR0–DR3/DR7 → `SetThreadContext` → `ResumeThread` on every thread in the process. Thread enumeration via `CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0)` with `Thread32First`/`Thread32Next` filtered by process ID handles existing threads. Newly created threads require either hooking `NtCreateThreadEx` or periodic re-application of breakpoints.

**Warden 3.3.5a detection risk: LOW.** Multiple researchers on OwnedCore confirmed that Warden in the 3.3.5a era **does not call `GetThreadContext` or `NtGetContextThread` to read debug registers** as part of its runtime scanning. Hardware breakpoint + VEH was the dominant, long-lived hooking technique in the WoW 3.3.5a bot community. The login sequence (battle.net.dll) does perform anti-debug checks — `NtQueryInformationProcess` with `ProcessDebugObjectHandle`, PEB `IsDebuggerPresent` checks, and rogue exception throws — but these are one-time login checks, not ongoing Warden scans. For defense in depth, hooking `NtGetContextThread` to zero out DR fields in the returned CONTEXT is straightforward insurance.

**Implementation complexity: MODERATE.** Requires VEH registration, thread enumeration, DR7 configuration, and careful RF flag handling — roughly 200–300 lines of focused C code. Well-documented with working implementations across multiple game hacking frameworks.

---

## 2. INT3 software breakpoints modify code and fail the core requirement

The INT3 opcode (`0xCC`) overwrites a single byte at the target function entry. When executed, it raises `EXCEPTION_BREAKPOINT` (`0x80000003`). Windows kernel adjusts EIP back by 1 before delivering the exception to usermode, so the VEH handler receives EIP pointing at the `0xCC` byte — identical to where the original instruction was. The handler can then read parameters, modify the buffer, restore the original byte, set EIP to the breakpoint address, and return `EXCEPTION_CONTINUE_EXECUTION` to execute the original function.

The single-byte modification is smaller than the 5-byte JMP used by inline hooks, but **it still fails the "no code modification" requirement**. Warden's MEM_CHECK reads raw bytes from specific addresses. If the INT3 sits at a scanned offset, the server receives `0xCC` instead of the expected byte — immediate detection. Even if the specific RC4 function address is not in Warden's scan database (and Warden 3.3.5a does not typically scan its own dynamically loaded module), the approach provides no structural guarantee against detection.

**INT3 is a trap, not a fault** — the CPU executes the INT3 and pushes EIP pointing to the *next* instruction onto the stack. The Windows kernel's `KiDispatchException` then decrements this by 1, so the VEH handler sees EIP at the INT3 location. This adjustment is specific to exception code `0x80000003` and does not apply to the long-form `INT 3` (`0xCD 0x03`, 2 bytes), which would leave EIP pointing into the middle of the instruction — a common source of bugs.

**Warden detection risk: HIGH** if at a scanned address. Warden's MEM_CHECK copies the byte and sends it to the server. **Implementation complexity: LOW** — simpler than hardware breakpoints since no thread enumeration or DR7 configuration is needed. However, the code modification disqualifies it from the stated requirements.

---

## 3. PAGE_GUARD hooks avoid code patches but suffer catastrophic performance

Setting `PAGE_GUARD` via `VirtualProtect` on the target function's page causes any access — read, write, or execute — to raise `STATUS_GUARD_PAGE_VIOLATION` (`0x80000001`). The VEH handler checks `ExceptionRecord->ExceptionAddress` against the target function address. On match, it modifies the buffer, redirects EIP, and uses a single-step trap to re-arm the guard. **No code bytes are modified**, only page protection metadata.

The critical flaw is the **one-shot nature combined with page-level granularity**. PAGE_GUARD is automatically removed after the first access to the page. Since it operates at the 4KB page boundary, *every instruction* executed on the same page as the target function triggers the exception — not just calls to the RC4 function. For a ~48KB allocation spanning ~12 pages, the target function's page likely contains hundreds of other instructions. Each execution triggers a two-exception cycle:

1. `STATUS_GUARD_PAGE_VIOLATION` fires → handler processes it, sets Trap Flag (`EFLAGS |= 0x100`)
2. `STATUS_SINGLE_STEP` fires after one instruction → handler calls `VirtualProtect` to re-arm PAGE_GUARD
3. Next instruction on the page triggers the cycle again

This creates **two kernel-to-usermode transitions per instruction** on the guarded page. Game hacking community sources characterize this flatly as "very very slow" and suitable only for functions called extremely rarely. For any code path executing more than a handful of instructions per second on the target page, the performance impact is unacceptable.

**Access type discrimination** is theoretically available via `ExceptionRecord->ExceptionInformation[0]` (0=read, 1=write, 8=execute on DEP-enabled systems), mirroring `EXCEPTION_ACCESS_VIOLATION` semantics. In practice, most implementations rely on comparing `ExceptionAddress` against the target rather than filtering by access type, since the guard fires on all access types regardless.

**Warden detection risk: MEDIUM.** Warden uses `VirtualQuery` to inspect memory regions, and `PAGE_GUARD` is visible in `MEMORY_BASIC_INFORMATION.Protect`. However, between the guard violation and re-arming, the flag is absent — creating a timing window where `VirtualQuery` would show normal permissions. Hooking `VirtualQuery` to hide the PAGE_GUARD flag is a viable countermeasure. **Implementation complexity: MODERATE** but performance makes this impractical for hot code paths.

---

## 4. Inline flickering trades race conditions for unlimited hook capacity

The flickering inline hook writes a 5-byte `JMP rel32` (`E9 xx xx xx xx`) at the function entry, immediately restores the original bytes after the hook fires, and re-installs the JMP before the next expected call. A trampoline — containing the saved original instructions plus a JMP back to `original+N` — allows the original function to be called cleanly.

The fundamental problem is the **race window with Warden's integrity scanning**. Warden's scan function uses `repe movsb` to copy bytes from target addresses into a buffer. If this memcpy executes while the JMP is installed, the modified bytes are captured and sent to the server. Warden scan timing is server-directed and not predictable from the client side, though scans typically arrive seconds to minutes apart.

**Atomic installation** is achievable for the 5-byte overwrite. The x86 `LOCK CMPXCHG8B` instruction atomically compares and swaps 8 bytes. By padding the 5 hook bytes with the 3 original trailing bytes, the entire write completes in a single bus-locked operation — preventing other threads from observing a partially written JMP instruction. The technique requires 8-byte alignment within a cache line. `FlushInstructionCache` must be called after modification to ensure instruction cache coherency.

**Synchronization with Warden scans** is the key challenge. Two viable strategies exist:

- **Hook the Warden scan function itself**: Pattern-scan the loaded Warden module for the `repe movsb` signature (`F3 A4 5F 5E C3`), install a detour on it, and substitute original bytes when the scan targets a hooked address. This was the dominant bypass technique on OwnedCore ("my protection works same like 5 months or more, still alive"). However, it requires an additional hook on Warden's code — itself detectable if Warden ever adds self-integrity checks.
- **Minimize hook installation time**: Only install the JMP when a call to the target function is imminent (e.g., monitor the call site) and remove it immediately after. This narrows but does not eliminate the detection window.

**Warden detection risk: MEDIUM-HIGH.** The race condition with integrity scanning is the primary concern. **Implementation complexity: HIGH** — requires a disassembly engine (Capstone, Zydis) for instruction-length decoding, trampoline construction with relative address fixups, atomic write primitives, and synchronization logic.

---

## 5. Function pointer hooks bypass code scanning entirely if the call is indirect

If the RC4 function is called through an indirect pointer — `CALL [register]` or `CALL DWORD PTR [memory]` — overwriting that pointer redirects calls without modifying any code bytes. This is the cleanest approach in principle: the target function's code region remains pristine, and only a **4-byte data pointer** changes. On x86-32, `InterlockedExchange` provides an atomic single-operation pointer swap.

The challenge is **applicability to Warden's architecture**. Warden modules are downloaded as encrypted PE-like structures, decrypted, and loaded into VirtualAlloc'd memory. They do not expose standard PE import/export tables in the conventional sense. However, Warden modules internally use function pointer tables and dispatch structures. If reverse engineering reveals that the RC4 function is called through a resolvable pointer in a data region, that pointer can be atomically swapped.

**Warden's MEM_CHECK scans target specific hardcoded code offsets** relative to known module bases. Data pointers in heap or stack structures are far less likely to be scanned. Warden does not perform general-purpose data integrity checking on its own module's internal structures — researchers confirmed they could modify Warden module internals without triggering detection.

There are two important sub-categories worth distinguishing:

- **IAT/EAT hooking** applies to standard PE modules with import/export tables. Not directly applicable to Warden's VirtualAlloc'd code block, which lacks conventional PE structure.
- **Virtual function table (vtable) or dispatch table hooking** applies if the Warden module uses object-oriented patterns or function pointer arrays internally. This requires reverse engineering the module's calling conventions to identify the pointer to overwrite.

**Warden detection risk: LOW** for data pointer modifications, since Warden's scanning focuses on code regions. **Implementation complexity: VARIES** — trivial once the target pointer is identified, but finding it requires significant reverse engineering of the obfuscated Warden module. The hook is also fragile: if the module layout changes between Warden module versions (which are downloaded dynamically), the pointer offset changes.

---

## 6. Shadow pages cannot achieve same-address read/execute split without a kernel driver

The shadow page concept aims to create two virtual address mappings to the same physical page: one "clean" view for integrity checks and one "patched" view for execution. If achievable, this would make hooks completely invisible to memory scanners reading from the same address that code executes from.

**Dual mapping from usermode is technically possible** via `NtCreateSection` (pagefile-backed, `SEC_COMMIT`) followed by two `NtMapViewOfSection` calls at different virtual addresses. Both views share the same physical pages — writes to one are immediately visible from the other. This technique is well-documented in offensive security for cross-process code injection (RW view locally, RX view in remote process).

**The fundamental limitation is that same-VA split-view is impossible in pure usermode.** Both mapped views share identical physical pages. If you patch one view, the other sees the same patch. To make reads and executes from the *same virtual address* return different data, you need page table manipulation that separates the instruction TLB (iTLB) from the data TLB (dTLB) — the "Shadow Walker" technique from Phrack 63. This requires:

- Kernel-mode access to manipulate page table entries (PTEs)
- `INVLPG` (a privileged instruction) to selectively flush TLB entries
- Or Extended Page Tables (EPT) via a hypervisor, using Intel VT-x to present different physical pages for read vs. execute accesses (the approach used by EPT-based hook hiding tools like Gbhv and matrix-rs)

Modern Intel CPUs further complicate TLB splitting: evicted iTLB/dTLB entries land in a unified secondary TLB (sTLB), defeating the split even from kernel mode on newer hardware.

**A weaker alternative exists**: create a patched copy at a different virtual address, then use another hooking mechanism (hardware breakpoint or PAGE_GUARD) at the original address to redirect execution to the patched copy. The original code remains intact for integrity scans reading the original address. But this still requires a detectable redirection mechanism — negating the shadow page's supposed advantage.

**Additional detection vector**: replacing a VirtualAlloc'd region (`MEM_PRIVATE`) with a section-backed mapping (`MEM_MAPPED`) changes the allocation type visible through `VirtualQuery`. Warden calls `VirtualQuery` to inspect memory regions, making this type change trivially detectable.

**Warden detection risk: HIGH** (type change detectable; requires detectable redirection mechanism). **Feasibility: NOT VIABLE** in pure usermode for the stated requirement. True split-view requires a kernel driver or hypervisor.

---

## 7. Instruction emulation is theoretically sound but practically infeasible

The emulation approach marks the target function's page as non-executable (`PAGE_READWRITE`), catching the resulting `EXCEPTION_ACCESS_VIOLATION` (with `ExceptionInformation[0] = 8`, indicating a DEP violation) in a VEH handler. The handler reads the faulting instruction's bytes as *data* (reads are still allowed on the NX page), decodes them, simulates their effect on the `CONTEXT` structure, advances EIP, and returns `EXCEPTION_CONTINUE_EXECUTION`.

**No code bytes are modified** — only page permissions change. The approach is conceptually elegant. But three compounding problems make it impractical:

**First, page-level granularity.** Making one page NX affects all code on that 4KB page. Every function, every instruction on the page must be emulated. This is not just the RC4 function — it's every piece of Warden module code sharing that page.

**Second, x86 instruction complexity.** The x86 ISA has variable-length instructions (1–15 bytes) with prefix bytes, ModR/M addressing modes, SIB bytes, and hundreds of opcodes spanning arithmetic, logic, memory operations, control flow, FPU, MMX, SSE, and more. Correctly emulating flag computation alone for all ALU instructions is a substantial engineering effort. Existing engines like Unicorn (based on QEMU) handle this but aren't designed to emulate individual instructions from a live process context via a VEH handler.

**Third, catastrophic performance.** Every instruction on the NX page triggers a kernel→usermode exception dispatch cycle. RedOps' published research on this technique deliberately used the simplest possible code (5 NOPs + RET) because even trivial instructions require careful emulation. For real-world Warden module code executing RC4 encryption with loops, memory accesses, and conditional branches, the overhead would freeze the process.

**Warden detection risk: MEDIUM** (permission change detectable via `VirtualQuery`). **Implementation complexity: EXTREME.** This is a research-grade proof of concept, not a practical hooking technique.

---

## How Warden 3.3.5a actually catches cheaters — and where the gaps are

Warden in WoW 3.3.5a is a **pure usermode, server-driven scanning system** with no kernel components. The server sends scan requests via encrypted `SMSG_WARDEN_DATA` packets, and the client-side Warden module executes them and returns results. The primary check types relevant to hooking detection are:

**MEM_CHECK** (`0xF3`): The most important check for code integrity. The server specifies a module name, memory offset, and byte length. The Warden module copies those bytes using `repe movsb` and sends them back. The server compares against expected values. This catches inline hooks, INT3 patches, and any byte-level code modification — but only at addresses in the server's scan database. Warden scans specific known addresses in WoW.exe (Lua protection at `0x494A57`, WS2_32.Send at `0x420541`, FrameScript functions, etc.) but **does not scan its own dynamically loaded module code**. Researchers confirmed they could detour the scan function within the Warden module without triggering detection.

**PAGE_CHECK** (`0xB2`/`0xBF`): SHA-1 hashes of memory pages, used to detect known cheat DLLs. PAGE_CHECK_B specifically scans pages starting with MZ+PE headers (loaded PE modules).

**MODULE_CHECK** (`0xD9`): Enumerates loaded modules via `Module32First`/`Module32Next` or PEB walking, comparing SHA-1 hashes against known cheat signatures.

**What Warden 3.3.5a does NOT do**: It does not read debug registers via `GetThreadContext`. It does not walk the VEH handler chain (`LdrpVectorHandlerList`). It does not perform self-integrity checks on its own module code. It does not monitor for PAGE_GUARD on its own code pages (though it does use `VirtualQuery` for other purposes). These gaps were confirmed by multiple independent researchers on OwnedCore and form the basis for the hardware breakpoint technique's long success.

---

## Conclusion: a ranked decision matrix

The evaluation reveals a clear hierarchy when all four criteria — Warden evasion, usermode feasibility, pre-execution buffer modification, and implementation complexity — are weighted together:

| Rank | Technique | Code Modified | Warden Risk | Performance | Complexity | Verdict |
|------|-----------|:---:|:---:|:---:|:---:|---------|
| **1** | **Hardware breakpoints + VEH** | None | **Low** | Good | Moderate | **Best choice** — proven in production |
| 2 | Function pointer hook | None (data only) | Low | Excellent | Varies† | Excellent if pointer is found |
| 3 | PAGE_GUARD + VEH | None | Medium | **Very poor** | Moderate | Only for rarely-called functions |
| 4 | Inline flicker + scan hook | Temporary | Medium-High | Good | High | Works but adds detection surface |
| 5 | INT3 + VEH | 1 byte | High | Good | Low | Fails no-modification requirement |
| 6 | Shadow page (dual map) | None‡ | High | Good | Extreme | Not feasible without kernel driver |
| 7 | Emulation + NX page | None | Medium | **Catastrophic** | Extreme | Research curiosity, not practical |

†Function pointer hooking complexity depends entirely on reverse engineering effort to locate the indirect call pointer — trivial to implement once found, but finding it requires deep analysis of the obfuscated Warden module.
‡True same-VA split-view is impossible in usermode; the approach fundamentally does not work as intended without kernel or hypervisor support.

**Hardware breakpoints win decisively** for this scenario. The 4-register limit exactly matches the requirement for up to 4 simultaneous hooks. The fault-before-execution semantics allow clean pre-execution buffer modification via CONTEXT manipulation. The RF flag provides a single-instruction suppression mechanism for clean resumption. And critically, Warden 3.3.5a's scanning architecture has a structural blind spot for debug register state — a gap that the WoW private server and bot community exploited successfully for years. The only defensive measure worth adding is a hook on `NtGetContextThread` to zero debug register fields in returned contexts, providing insurance against any future Warden module update that might add debug register scanning.