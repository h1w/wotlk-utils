# Warden Bypass — Реализация

Система перехвата, анализа и обхода команд Warden (античит) для WoW 3.3.5a (Build 12340).
Работа ведётся исключительно на стороне клиента через инжектированную DLL.

Последнее обновление: 2026-02-16.

---

## Архитектурный обзор

```
Сервер
   │
   ├── SMSG_WARDEN_DATA (opcode 0x2E6)
   │     │
   │     ├── MODULE_USE (0x00)        ──► module_dump: сохранить на диск / загрузить из кеша
   │     ├── MODULE_CACHE (0x01)      ──► module_dump: собрать чанки → RC4 → zlib
   │     ├── MODULE_INITIALIZE (0x02) ──► warden_scan: извлечь type IDs из in-memory модуля
   │     │                                warden_rc4_hook: установить RC4 hook внутри модуля
   │     ├── HASH_REQUEST (0x05)      ──► логирование (spoofing не реализован)
   │     └── CHEAT_CHECKS_REQUEST     ──► hooks.cpp: парсинг → warden_spoof: push в FIFO queue
   │           ├── TIMING             ──► pass-through (5 bytes)
   │           ├── MEM_CHECK          ──► warden_spoof: shadow_copy replacement
   │           ├── PAGE_CHECK         ──► warden_spoof: force 0xE9 (pass)
   │           ├── MODULE_CHECK       ──► peb_unlink (Layer 1) + warden_spoof force 0xE9 (Layer 2)
   │           ├── LUA_EVAL           ──► warden_spoof: селективный spoof (addon-detection)
   │           ├── DRIVER_CHECK       ──► pass-through
   │           ├── MPQ_CHECK          ──► pass-through
   │           └── PROC_CHECK         ──► pass-through
   │
   ├── CMSG_WARDEN_DATA (opcode 0x2E7)
   │     └── warden_rc4_hook         ──► перехват plaintext ПЕРЕД RC4 шифрованием
   │           └── warden_spoof      ──► SpoofCmsgIfNeeded → modify → recompute checksum
   │
   └── FrameScript_Execute (0x00819210) ──► hooks.cpp: логирование Lua вызовов
```

---

## Реализованные компоненты

### 1. Перехват SMSG_WARDEN_DATA (hooks.cpp)

**Адрес**: 0x007DA850 (build 12340)

**Проблема**: НЕ стандартный __thiscall. Stack layout: `[ret] [0] [opcode=0x2E6] [ECX_dup] [CDataStore*]`. Данные RC4-зашифрованы на момент входа в handler — дешифровка происходит ВНУТРИ.

**Решение**: `__declspec(naked)` detour + return-address hijack:
1. Pre-handler: сохраняет CDataStore* и readPos, подменяет return address
2. Оригинальный handler: расшифровывает пакет (RC4)
3. Post-handler (WardenPostHandler): читает расшифрованные данные через CDataStore

**Парсинг**:
- MODULE_USE → module_dump: SHA1 hash + RC4 key
- MODULE_CACHE → module_dump: чанки модуля
- MODULE_INITIALIZE → warden_scan + warden_rc4_hook
- HASH_REQUEST → 16-byte challenge seed (логирование)
- CHEAT_CHECKS_REQUEST → ParseCheatChecksRequest → PushPendingChecks

### 2. Перехват SendPacket (hooks.cpp)

**Адрес**: 0x00632B50

**Назначение**: `__declspec(naked)` hook, перехватывает CMSG_WARDEN_DATA (opcode 0x2E7). Payload RC4-зашифрован — используется для логирования размеров и timing, но не для чтения plaintext.

### 3. Извлечение типов из модуля (warden_scan.cpp)

Каждый Warden модуль использует свои module-specific type IDs. Идентификация через:

1. **XOR-anchored scan**: паттерн `xor r8, [reg+4]` → `movzx eax, al`
2. **Dispatch chain walker**: BFS по дереву сравнений (CMP + conditional jumps)
3. **Remap table supplement**: cross-reference нескольких remap таблиц (group threshold <= 5)
4. **Dynamic type discovery**: `TryAssignSize` при парсинге с квотами (`kSizeMaxCount[]`)
5. **2-step lookahead**: fallback если structural validation не работает

**Приоритеты**: in-memory scan → RLE-unpacked binary → raw packed → blind MEM_PRIVATE scan

### 4. RC4 Hook внутри модуля (warden_rc4_hook.cpp)

**Проблема**: Warden payload RC4 — ВНУТРИ модуля (не session cipher 0x00774EA0).

**Решение**: хук на RC4 PRGA функцию внутри VirtualAlloc-блока модуля.
- Сканер ищет кластеры `push ebp; mov ebp, esp` с обращениями к offset 0x100/0x101 (i/j)
- До 4 хуков одновременно (разные RC4 функции для main thread и module thread)
- Auto-detection calling convention (6 вариантов: ECX/EDX/EBX/ESI/EDI/EAX)
- `ConsumePlaintext()`: one-shot retrieval CMSG plaintext перед RC4 encrypt
- `SpoofCmsgIfNeeded()` вызывается ДО RC4 encrypt → модифицированный plaintext шифруется как "родной"

**Fallback**: S-box cloning (warden_rc4.cpp) — сканирование MEM_PRIVATE памяти для RC4 S-box permutations

### 5. Spoofing (warden_spoof.cpp)

**Variant A**: модификация CMSG plaintext в RC4 hook, перед шифрованием.

**FIFO queue**: `std::deque<vector<PendingCheck>>` — корреляция SMSG requests → CMSG responses.

**SpoofCmsgIfNeeded()** — copy-based rebuild CMSG results:
- **MEM_CHECK**: если адрес пересекается с hook target → замена данных на чистые байты из shadow_copy
- **PAGE_CHECK**: если адрес пересекается с hook target → force 0xE9 (pass)
- **MODULE_CHECK**: если result != 0xE9 → force 0xE9 (backup к PEB unlinking)
- **LUA_EVAL**: если eval code содержит addon-detection API + результат непустой → замена на empty string
- **Checksum recompute**: `warden_checksum::BuildChecksum()` после модификации

### 6. PEB Unlinking (peb_unlink.cpp)

**Цель**: скрыть DLL из PEB.Ldr lists → CreateToolhelp32Snapshot не видит модули.

**Что скрываем**: wotlk.dll, glog.dll, gflags_debug.dll / gflags.dll

**API**:
- `UnlinkAll(hModule)` — вызывается ПОСЛЕ всей инициализации
- `RelinkAll()` — вызывается ПЕРЕД eject (FreeLibraryAndExitThread)

### 7. Shadow Copy (shadow_copy.cpp)

PE mapping Wow.exe с диска → чистые байты .text секции (без hook patches).

---

## Calling Conventions хуков

| Адрес | Функция | Convention | Detour |
|-------|---------|-----------|--------|
| 0x00819210 | FrameScript_Execute | `__cdecl` | MinHook trampoline |
| 0x007DA850 | SMSG_WARDEN_DATA handler | нестандартная | `__declspec(naked)` + ret hijack |
| 0x00632B50 | SendPacket | нестандартная | `__declspec(naked)` + pushad/popfd |
| 0x00774EA0 | ARC4::Process | `__thiscall` | MinHook trampoline (diagnostics only) |
| module RC4 | RC4 PRGA (inside module) | varies | naked stubs × 4 + auto-detect |

---

## Зависимости (vcpkg)

| Пакет | Triplet | Назначение |
|-------|---------|-----------|
| `glog` | x86-windows | Логирование |
| `minhook` | x86-windows-static-md | Inline hooking |

Также: `miniz` (исходники в third_party/) для zlib decompression модулей.

---

## Что НЕ реализовано

- **HASH_REQUEST spoofing**: алгоритм хеша внутри модуля не реверсирован
- **DRIVER_CHECK/MPQ_CHECK/PROC_CHECK spoofing**: pass-through (не нацелены на нашу DLL)
- **VirtualQuery hooking**: не нужен (MinHook восстанавливает page protection после патча)
