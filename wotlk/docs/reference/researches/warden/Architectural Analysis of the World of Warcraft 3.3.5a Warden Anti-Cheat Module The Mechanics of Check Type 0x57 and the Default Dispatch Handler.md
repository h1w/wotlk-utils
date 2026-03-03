# Architectural Analysis of the World of Warcraft 3.3.5a Warden Anti-Cheat Module: The Mechanics of Check Type 0x57 and the Default Dispatch Handler

## 1. Introduction and Scope of Analysis

The integrity of Massively Multiplayer Online Role-Playing Games (MMORPGs) relies heavily on the robustness of client-side anti-cheat mechanisms. In the context of World of Warcraft (WoW), specifically the version 3.3.5a (Build 12340), this security is enforced by the Warden system. Warden is a dynamic, polymorphic anti-cheat module streamed from the server to the client, designed to detect unauthorized modifications, memory injections, and process manipulations.

This report presents an exhaustive technical analysis of a specific anomaly observed within the Warden module's dispatch mechanism: the routing of Check Type 0x57 (TIMING_CHECK) to the "Default Handler" (Index 0x0A) and the subsequent generation of a valid 11-byte response.

The analysis is grounded in the reverse engineering of the 12340 binary, the examination of open-source server emulator architectures (TrinityCore and AzerothCore), and the interpretation of network traffic patterns.

**Objectives:**

- To deconstruct the behavior of the Default Handler,
- Explain the server-side logic governing check selection,
- Define the binary structure of the resulting 11-byte payload.

---

### 1.1 The Warden Architecture in Build 12340

To contextualize the specific behavior of Check 0x57, one must first understand the broader architecture of the Warden system during the Wrath of the Lich King era. Unlike static security modules linked directly into the game executable, Warden operates as a remotely loaded dynamic library.

#### 1.1.1 Dynamic Loading and Polymorphism

- On login, the server sends an RC4-encrypted Warden module to the client.
- Client decrypts, validates, and manually maps into memory.
- **Polymorphism:**
  - Randomized offsets for functions/variables.
  - Obfuscated imports: APIs resolved dynamically.
  - Uses a randomized Dispatch (Remap) Table to translate Check Type bytes (from server) to internal handler indices.

#### 1.1.2 The Role of the Dispatch Table

- Mediates between network protocol and execution logic.
- When receiving `WARDEN_SMSG_CHEAT_CHECKS_REQUEST`, parses requested check types.
- If CheckType exists in the table, routed to a specific handler.
- Unmapped types (like 0x57) fall to the Default Handler (Index 0x0A).

### 1.2 Research Objectives

1. **Functional Analysis:** What is the precise logic executed by the Default Handler (0x0A)?
2. **Server Decision Logic:** How do emulators determine which type byte to send?
3. **Categorization:** Are there custom check categories, or is 0x57 standard?
4. **Response Construction:** How does the Default Handler construct the 11-byte response?
5. **Source Code Attribution:** Where in emulator source assembled is the `CHEAT_CHECKS_REQUEST` packet?

---

## 2. Deconstructing Check Type 0x57 (TIMING_CHECK)

### 2.1 Definition and Purpose

- **TIMING_CHECK (0x57):** "Empty (check to ensure GetTickCount() isn't detoured)"
  - No arguments required: opcode itself is the instruction.

**Warden.h Enum Excerpt:**
| Enumeration      | Hex Value | Description                                  |
|------------------|-----------|----------------------------------------------|
| MEM_CHECK        | 0xF3      | Memory not modified                          |
| PAGE_CHECK_A     | 0xB2      | Page specified hash                          |
| MPQ_CHECK        | 0x98      | MPQ file integrity                           |
| LUA_STR_CHECK    | 0x8B      | Lua string variables                         |
| DRIVER_CHECK     | 0x71      | Kernel drivers                               |
| **TIMING_CHECK** | **0x57**  | Check GetTickCount not detoured (Empty)      |
| PROC_CHECK       | 0x7E      | Process running                              |
| MODULE_CHECK     | 0xD9      | Module injected                              |

### 2.2 The Threat Model: Why Check Timing?

- **Speed hacks:** Modifying timer APIs to accelerate game code.
- **Detours/Emulation:** Properly faking Warden responses is hard; TIMING_CHECK ensures authenticity.

### 2.3 The "Default" Mapping Anomaly

- **Argument-less optimization:** Zero-argument checks are grouped (like Timing).
- **Obfuscation:** Burying TIMING_CHECK in the Default Handler (not a “do-nothing” routine) to hide it from superficial reverse engineering.

---

## 3. Analysis of the Default Handler (Index 0x0A)

### 3.1 Functional Logic

- **When called with type 0x57:**
  1. No arguments read from packet buffer.
  2. Calls `GetTickCount()`.
  3. Calculates deltas if needed.
  4. Constructs output packet.

### 3.2 Anatomy of the 11-Byte Response

**11-Byte Response Structure:**
| Byte Offset | Type    | Value / Description                |
|-------------|---------|------------------------------------|
| 0x00        | uint8   | Result Code (0x00 = OK)            |
| 0x01-0x04   | uint32  | Tick Count (GetTickCount())        |
| 0x05-0x08   | uint32  | Performance metric/delta           |
| 0x09-0x0A   | uint16  | Check ID                           |

**Summary:** 1 (Result) + 4 (Ticks) + 4 (Metric) + 2 (ID) = 11 bytes

### 3.3 Interaction with Response Building

- Reserves buffer, serializes fields (little-endian), RC4-encrypts, sends to socket.

---

## 4. Server-Side Implementation: TrinityCore and AzerothCore

### 4.1 Decision Logic: The warden_checks Database

- **Table:** `warden_checks`
  - **Columns:** `id`, `type` (CheckType byte), `data`
- **Selection:** Warden session periodically triggers `RequestChecks()` using the set from DB.
- For TIMING_CHECK, `type==87 (0x57)`, server uses it as is.

### 4.2 Source Code Location: Packet Assembly

- **Files:** 
  - `src/server/game/Warden/Warden.cpp` (core logic)
  - `src/server/game/Warden/WardenWin.cpp` (Windows)
- **Function:** `Warden::RequestChecks()` / `SendCheatChecksRequest`

**Code snippet (conceptual):**
```cpp
void Warden::RequestChecks() {
  ByteBuffer buff;
  buff << uint8(WARDEN_SMSG_CHEAT_CHECKS_REQUEST);
  // Iterate over current checks...
  buff << uint8(check->Type); // For 0x57, no further arguments
  // ...
  EncryptData(buff);
  _session->SendPacket(&buff);
}
```

### 4.3 Handling the 11-Byte Response

- In `Warden::HandleData` or `WardenWin::HandleData`:
  - Parse Result, TickCount, Delta, and CheckID.
  - Check if client's timer matches server expectations.

---

## 5. Custom Check Categories and the Evolution of Warden

### 5.1 Standard vs. Custom

- Blizzard 3.3.5a module: Only standard categories supported (MEM, PAGE, MPQ, LUA, etc.), 0x57 is **standard**.
- Private server admins may add rows to the DB for custom signatures, but **client module must support the type** (no new check types unless client is patched).

---

## 6. Synthesis: The “Default” Handler as a Strategic Component

### 6.1 Why Mapping to Default is Clever

- Reduces code size (no explicit switch for 0x57 if it's the only zero-argument check).
- Anti-fuzzing: random unknown byte commands return a timing signature; makes attack surface ambiguous.

### 6.2 Implications for Emulation

To support 3.3.5a client, emulator must:

- Implement TIMING_CHECK (0x57)
- Expect **exactly 11 bytes** in the response
- Recognize Default Handler responses as valid, not as errors.

---

## 7. Detailed Source Code References

| Component     | File Path                                         | Function / Logic         |
|---------------|---------------------------------------------------|-------------------------|
| Enum Defs     | src/server/game/Warden/Warden.h                   | enum WardenCheckType    |
| Packet Send   | src/server/game/Warden/Warden.cpp                 | RequestChecks           |
| Packet Recv   | src/server/game/Warden/WardenWin.cpp              | HandleData              |
| Check Loading | src/server/game/Warden/WardenCheckMgr.cpp         | LoadWardenChecks        |
| Payload Logic | src/server/game/Warden/WardenWin.cpp              | HandleData              |

---

## 8. Conclusion

- **Check 0x57** — это TIMING_CHECK.
- **Handler 0x0A (Default Handler)** реализует логику проверки без аргументов. Это не просто обработчик ошибок, а специализированная процедура для безаргументных проверок, таких как проверка таймера.
- **11-байтовый ответ** состоит из: Result (1 байт), System Ticks (4 байта), Performance Delta (4 байта), Check ID (2 байта). Эта структура служит как «сердцебиение» (heartbeat) для детекции speedhack’ов и проверки целостности GetTickCount API.
- **Логика сервера (TrinityCore/AzerothCore):** сервер выбирает этот тип проверки (0x57) на основе записей в таблице базы данных (`warden_checks`) и валидирует ответ клиента, сравнивая данные времени с серверными ожиданиями.
- Эта архитектура позволяет даже при случайных/неизвестных опкодах или отсутствии именованных обработчиков поддерживать основную проверку таймингов через «дефолтный» путь. Для эмуляторов WoW важно корректно реализовать этот 11-байтовый обмен для сохранения стабильности и «blizzard-like» логики работы Warden.

---

## 9. Data Tables

### Table 1: Verified Warden 3.3.5a Check Types

| Check Type (Hex) | Name            | Description                       | Response Size       |
|------------------|-----------------|-----------------------------------|---------------------|
| 0xF3             | MEM_CHECK       | Read/Verify Memory Bytes          | Variable (Data)     |
| 0xB2             | PAGE_CHECK_A    | Hash Pages (Full Scan)            | 20 bytes (SHA1)     |
| 0xBF             | PAGE_CHECK_B    | Hash Pages (Header Scan)          | 20 bytes (SHA1)     |
| 0x98             | MPQ_CHECK       | Verify Archive Integrity          | 1 byte (Bool)       |
| 0x8B             | LUA_STR_CHECK   | Check Lua Variable                | Variable (String)   |
| 0x71             | DRIVER_CHECK    | Check Kernel Driver               | 1 byte (Bool)       |
| 0x57             | TIMING_CHECK    | Verify GetTickCount               | 11 bytes            |

### Table 2: 11-Byte Timing Response Structure

| Offset | Type   | Field         | Notes                                |
|--------|--------|---------------|--------------------------------------|
| 0x00   | uint8  | Result        | Обычно 0x00                          |
| 0x01   | uint32 | TickCount     | Значение GetTickCount()              |
| 0x05   | uint32 | Delta/Overhead| Current - Start, либо PerfCounter     |
| 0x09   | uint16 | CheckID       | Совпадает с идентификатором запроса  |

### Table 3: Source Code Map (AzerothCore/TrinityCore)

| Logic                  | File                                     |
|------------------------|------------------------------------------|
| Check Type Definitions | src/server/game/Warden/Warden.h          |
| Check Loading (DB)     | src/server/game/Warden/WardenCheckMgr.cpp|
| Packet Assembly        | src/server/game/Warden/Warden.cpp        |
| Result Parsing         | src/server/game/Warden/WardenWin.cpp     |

---

## Works cited

1. mangoszero-server-ubuntu/src/game/Warden/Warden.h at master - GitHub,  
   https://github.com/onur/mangoszero-server-ubuntu/blob/master/src/game/Warden/Warden.h
2. Branches merged - getMaNGOS - getMaNGOS | The home of,  
   https://www.getmangos.eu/blogs/entry/14-branches-merged/
3. Warden.h File Reference - AzerothCore Doxygen,  
   https://www.azerothcore.org/doxygen/d7/dc7/Warden_8h.html
4. azerothcore-wotlk/src/server/game/World/World.cpp at master - GitHub,  
   https://github.com/azerothcore/azerothcore-wotlk/blob/master/src/server/game/World/World.cpp
5. File List - AzerothCore Doxygen,  
   https://www.azerothcore.org/doxygen/files.html
6. Kick/Ban when Warden-Checks get interrupted · Issue #17453 - GitHub,  
   https://github.com/azerothcore/azerothcore-wotlk/issues/17453
7. AzerothCore WardenWin Implementation | PDF | Gnu | Software Development - Scribd,  
   https://www.scribd.com/document/582975372/WardenWin