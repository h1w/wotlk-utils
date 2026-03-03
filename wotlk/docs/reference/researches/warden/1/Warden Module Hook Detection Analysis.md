# Advanced Analysis of Warden  
## Anti-Cheat Module Integrity Mechanisms and Detection Heuristics in World of Warcraft 3.3.5a

### 1. Introduction to the Warden Architecture and Operational Paradigm

The deployment of dynamic, server-streamed anti-cheat modules represents a foundational pillar of the Warden security architecture within the World of Warcraft 3.3.5a (build 12340) client environment. These modules are specifically engineered to operate as highly obfuscated, custom binary blobs rather than conforming to the standard Portable Executable (PE) or Dynamically Linked Library (DLL) file formats utilized by traditional Windows applications.[1] This architectural divergence is a deliberate defensive mechanism designed to frustrate static analysis, inhibit the use of standard reverse-engineering toolchains, and seamlessly integrate into the volatile memory space of the host client...

*Пропускаю общий текст для краткости, далее полный Markdown — если нужна вся простыня, дай знать.*

---

### 1.1 Custom Binary Blob Structure and Cryptographic Unpacking Pipeline

Warden modules deployed within the 3.3.5a client environment are subjected to profound layers of obfuscation, compression, and encryption.[1] Unlike conventional PE files that rely on the Windows loader (LdrLoadDll) for mapping and initialization, these custom blobs utilize a proprietary 40-byte header, Run-Length Encoding (RLE) for section packing, and sophisticated delta-encoded relocations. This architecture dictates that the module cannot be analyzed natively by standard disassemblers without an intermediate, highly specialized unpacking layer.

The initialization pipeline of a Warden module follows a strict, multi-stage cryptographic validation sequence before execution is permitted within the client's allocated memory space. This sequence is highly serialized to guarantee that the payload has not been subjected to tampering, interception, or man-in-the-middle attacks during transit from the authentication server.[3]

#### Pipeline Stages

| Pipeline Stage                       | Cryptographic / Structural Operation         | Functional Description and Mechanism of Action                                                                                       |
|--------------------------------------|--------------------------------------------|-------------------------------------------------------------------------------------------------------------------------------------|
| **1. MD5 Verification**              | $H_{MD5}(Payload)$                         | Validates the raw, encrypted payload against a server-provided hash challenge to ensure network transmission integrity prior to cryptographic decryption. |
| **2. RC4 Decryption**                | $PRGA(State, Key) \oplus$                  | Decrypts the module payload utilizing an RC4 stream cipher. The symmetric key is dynamically derived from the server challenge parameters issued during the initial handshake.[3] |
| **3. RSA-2048 Signature**            | $S^e$                                      | Verifies the cryptographic signature of the decrypted payload. The 256-byte modulus ($n$) is securely hardcoded into the WoW.exe client binary at offset 0x005e3a03.[3]           |
| **4. zlib Decompression**            | Deflate Algorithm                          | Expands the RLE-packed and zlib-compressed binary sections into their true volumetric footprint, preparing the data for memory mapping.[3]                                     |
| **5. Header Parsing**                | Proprietary Header Analysis                | Reads the custom 40-byte header to identify executable code sections, .data segments, and the locations of the delta-encoded relocation tables.[3]                             |
| **6. Base Relocation**               | Delta-Encoded Offset Patching              | Applies necessary relocations so the module code can execute cleanly at the arbitrary base memory address assigned by the OS’ virtual memory manager.                         |
| **7. Import Binding**                | Dynamic API Resolution                     | Resolves necessary Windows API stubs (e.g., VirtualAlloc, Sleep) alongside internal WoW client function callbacks required for operation.[3]                                   |
| **8. Initialization**                | Execution of Entry Point                   | Transfers execution control to the module's entry point, commencing background worker thread creation and the initiation of environment scanning.[3]                            |

---

### 1.2 Virtual Memory Allocation and the PAGE_EXECUTE_READWRITE Vulnerability Paradigm

To successfully facilitate the dynamic unpacking, decompression, and base relocation of the custom binary blob, the WoW client allocates a single, contiguous block of virtual memory—typically 45–49 KB—via the VirtualAlloc Windows API function.[3] **This allocation must be instantiated with PAGE_EXECUTE_READWRITE (RWX) memory protection.**

RWX is functionally necessary for a module that unpacks, relocates, and executes code dynamically without the Windows loader. However, RWX memory is also highly susceptible to inline API hooking (e.g., via MinHook), as code can be overwritten without VirtualProtect (which itself can be monitored by anti-cheat systems). Frameworks like MinHook overwrite the target function's prologue with a 5-byte relative JMP (opcode 0xE9), redirecting execution to a custom trampoline.[6]

---

## 2. Implementations of Runtime Self-Integrity Verification Mechanisms

The deferred hooking of the RC4 PRGA function triggers a local detection, indicating the FC6560DA module inspects its own volatile memory through *internal, runtime self-integrity checks*, not dependent on the server’s polling.[4]

### 2.1 Cryptographic Hashing of Active Code Segments

Warden modules calculate SHA-1 (preferred for cryptographic rigor) or CRC32 (used for lightweight, localized checks) hashes over strict boundaries of executable code—not the whole binary, for performance.[8][4]

```math
H_i = f(H_{i-1}, M_i)
```
Where $M_i$ is the 512-bit message block from memory.

If MinHook installed a 5-byte `$0xE9$` patch, this will be reflected in the hash and trigger detection immediately.[1]

### 2.2 Instruction Pattern Scanning Heuristics

Warden additionally uses direct byte-pattern scanning in memory, looking for:

1. **0xE9 (JMP Unconditional):** Typical of inline hooks (MinHook, Detours, etc.), used to redirect code.
2. **0xCC (INT 3):** Debugger breakpoints.

Warden scans critical function pointers (like RC4 PRGA), dereferences the first 1–5 bytes, and if it finds 0xE9, flags tampering.[1]

### 2.3 Structural Assembly Patterns of Internal Self-Check Routines

Warden’s self-checks are purely internal, do not rely on OS APIs like ReadProcessMemory, and work by reading memory directly, hashing/pattern scanning, and comparing the result to a hardcoded value in .data.

If a mismatch is found, an internal flag triggers enforcement.

---

## 3. Threading Models: Worker Threads versus System Timers

The observed delay (up to 4 minutes) from a hook install to detection suggests dedicated background worker threads, not polling on the main thread or OS-level timers. These threads awaken on schedule, run checks (e.g., scan memory, compute hashes), then sleep again to minimize performance impact.[14][17]

### 3.2 Analytical Deconstruction of the 4-Minute Detection Latency

- **Hypothesis A:** Uses a single long Sleep (e.g., 240000 ms).
- **Hypothesis B:** Employs a slow, rolling scan, e.g., 16 bytes every 80 ms.
- **Hypothesis C:** Requires *three consecutive positive detections* before disconnecting (3-strike logic).

---

## 4. The Asynchronous Hashing Hypothesis: Analyzing Timing Race Conditions

If hash computation is performed asynchronously, installing hooks after the server’s HASH_REQUEST but before the worker’s memory scan means the scan will see the tainted memory, resulting in detection.

Let:
- $T_{req}$: Time of HASH_REQUEST receipt
- $T_{hook}$: Time hook is installed
- $T_{hash}$: Time memory is actually read for hashing

If $T_{req} < T_{hook} < T_{hash}$, the hash will include the hack and trigger detection.

---

## 5. Alternative Detection Vectors Beyond Direct Code Integrity

Warden may also detect:

### 5.1 MinHook Trampoline Artifact Scanning and Proximity Detection

- Scans for “trampolines” (allocated executable memory) adjacent to patched regions; checks for relative jumps back into module memory.
- Watches for suspicious VirtualAlloc calls after init.

### 5.2 Page Protection Anomalies via NtQueryVirtualMemory

If page protections are changed to hide hooks, Warden can spot this via queries and detect inconsistencies.

### 5.3 Vectored Exception Handling (VEH) Profiling and Chain Traversal

Checks registered VEH handlers for anomalies pointing to outside memory regions.

### 5.4 Hardware Breakpoint Inspection via Context Retrieval

Reads debug registers (DR0–DR3, DR7) with GetThreadContext; any set breakpoints in critical locations are flagged.

---

## 6. Detection Signaling and Lua FrameScript Client Callbacks

Modern modules like FC6560DA do **not** rely on server-side kicks. Instead, upon local detection, they execute a Lua FrameScript callback to display warning popups and ultimately disconnect the user.

Memory offsets for relevant client functions in 3.3.5a:

| Address (3.3.5a) | Function      | Description |
|------------------|--------------|-------------|
| 0x48D4A0         | AddChatMessage         | System message to chat log   |
| 0x490430         | SendChatMessage        | Network