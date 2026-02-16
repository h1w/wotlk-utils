# Статус проекта wotlk-utils

Этот документ показывает текущее состояние проекта: что уже работает, что не работает, и к чему мы стремимся.

Последнее обновление: 2026-02-16.

---

## Что уже работает (DONE)

### 1. Инжекция и выгрузка DLL

**Описание**: базовая инфраструктура для загрузки и выгрузки нашего кода в процесс WoW.

**Что работает**:
- Injector (консольное приложение x86) инжектирует DLL в Wow.exe (32-bit процесс)
- DLL загружается с флагом `LOAD_WITH_ALTERED_SEARCH_PATH` (чтобы найти зависимости)
- DLL открывает консоль (`AllocConsole`) с поддержкой ANSI цветов
- Устанавливаются хуки (MinHook)
- Логирование через glog (custom sink для цветного вывода в консоль)

**Выгрузка**:
- `injector.exe --eject` создаёт named event (`wotlk_unload_event`)
- DLL детектирует event, снимает хуки, закрывает консоль
- `FreeLibraryAndExitThread` для безопасного self-unload (без deadlock в DllMain)
- Консоль корректно закрывается (fclose stdin/stdout/stderr перед FreeConsole)

**Статус**: ПОЛНОСТЬЮ РАБОТАЕТ

---

### 2. Перехват FrameScript_Execute

**Описание**: хук на функцию выполнения Lua скриптов в клиенте WoW.

**Адрес**: 0x00819210 (build 12340)

**Сигнатура**: `void __cdecl FrameScript_Execute(const char* code, const char* filename, int unused)`

**Что работает**:
- Логирование всех нестандартных Lua вызовов
- Фильтрация UI скриптов (по маске `Interface\FrameXML\*`, `Interface\AddOns\*`)
- Цветной вывод в консоль (зелёный для обычных, жёлтый для подозрительных)

**Применение**: детектирование LUA_EVAL проверок от Warden (например, проверка на запрещённые аддоны)

**Статус**: ПОЛНОСТЬЮ РАБОТАЕТ

---

### 3. Перехват SMSG_WARDEN_DATA

**Описание**: перехват пакетов от сервера к клиенту, содержащих команды Warden.

**Адрес**: 0x007DA850 (build 12340)

**Calling convention**: НЕ стандартный __thiscall! Используем `__declspec(naked)` + pushad/popfd.

**Механизм**: return-address hijack
- Pre-handler: сохраняет CDataStore* и readPos, подменяет return address
- Оригинальный handler: расшифровывает пакет
- Post-handler: читает расшифрованные данные

**Что работает**:
- Чтение расшифрованных данных (ПОСЛЕ обработки handler'ом)
- Парсинг ВСЕХ типов пакетов:
  - **MODULE_USE** (0x00): запрос загрузки модуля (SHA1 hash + 16-byte RC4 key)
  - **MODULE_CACHE** (0x01): чанк модуля (MD5 hash + chunk data)
  - **MODULE_INITIALIZE** (0x02): конец загрузки модуля
  - **HASH_REQUEST** (0x05): запрос хеша (16-byte challenge seed)
  - **CHEAT_CHECKS_REQUEST** (0x02 после initialize): массив проверок
- Извлечение строк (LUA_EVAL code, DRIVER name) с правильной обработкой string section terminator (0x00)
- Извлечение xorByte (последний байт пакета) — XOR для type bytes
- Извлечение типов проверок (TIMING, MEM, PAGE_A, PAGE_B, MPQ, LUA, DRIVER, PROC, MODULE)
- Извлечение адресов хуков через ScanForHookAddresses (module-agnostic)

**Статус**: ПОЛНОСТЬЮ РАБОТАЕТ

---

### 4. Захват модулей Warden

**Описание**: сохранение бинарных модулей Warden на диск для анализа.

**Механизм**:
1. Сервер отправляет MODULE_USE с SHA1 hash и RC4 ключом
2. Сервер отправляет серию MODULE_CACHE пакетов (чанки модуля)
3. Мы собираем чанки в буфер
4. Расшифровываем RC4 (ключ из MODULE_USE)
5. Распаковываем zlib (miniz library)
6. Сохраняем на диск:
   - `[module_hash]_encrypted.bin` — как пришло от сервера
   - `[module_hash]_decrypted.bin` — после RC4
   - `[module_hash]_decompressed.bin` — после zlib
   - `[module_hash]_meta.txt` — MD5, размеры, ключ

**Кэширование**:
- При reconnect пытаемся загрузить модуль из кеша (`TryLoadFromCache`)
- Если сервер отправляет только MODULE_USE (без MODULE_CACHE) — используем cached версию

**Формат модуля** (НЕ PE!):
- 40-byte header: moduleSize, relocOff, relocCount, exportTableOff, exportCount, baseIndex, importTableOff, importLibCount, sectionDescCount
- Section descriptors: 12 байт на секцию (RLE-packed)
- Relocation table: delta-encoded
- In-memory: VirtualAlloc с PAGE_EXECUTE_READWRITE, MEM_PRIVATE

**Статус**: ПОЛНОСТЬЮ РАБОТАЕТ (24 модуля захвачено, 21 с decompressed бинарниками)

---

### 5. Извлечение типов проверок из модуля

**Описание**: определение module-specific check type IDs из бинарного модуля.

**Проблема**: каждый модуль использует свои ID для типов проверок (не совпадают с TC константами).

**Решение**: In-memory scan (primary) + RLE-unpacked binary scan (fallback) + XOR-anchored dispatch chain + remap table supplement + dynamic type discovery

**Приоритеты сканирования**:
1. **In-memory scan** (`ScanModuleInMemory`): сканирование загруженного модуля в памяти процесса
   - Модуль уже распакован (RLE) и релоцирован — содержит реальный x86 код
   - `g_scanBaseAddr` корректирует абсолютные адреса (remap tables) в buffer-relative offsets
2. **RLE-unpacked binary scan** (`ScanModuleBinary` с `UnpackRLE`): распаковка decompressed дампа
   - Парсит 40-byte header + section descriptors, выполняет RLE unpack
   - Результат = runtime-ready image без RLE control word артефактов
3. **Raw packed binary scan** (legacy fallback): сканирование packed binary напрямую
4. **Blind memory scan** (`ScanRegionForDispatcher` → `ScanAndExtractTypeIDs`): последний resort — все MEM_PRIVATE регионы
   - `g_scanBaseAddr` устанавливается перед сканированием для корректировки абсолютных displacement'ов в релоцированном коде

**Алгоритм извлечения типов**:
1. **Anchor search**: ищем паттерн `xor r8, [reg+4]` (опкод `32 [40-7F, rm!=4] 04`)
2. **movzx detection**: следующая инструкция `movzx eax, al` (расширяет type до 32-bit)
3. **Dispatch chain walker**: BFS queue-based обход дерева сравнений
   - **BFS с visited set**: max 16 branches, 400-byte scan limit, per-branch accumulator
   - **Union of ALL dispatch chains**: если модуль имеет несколько цепочек, объединяем типы
   - **CMP values always inserted**: для greater/less family jumps (BST pivots are real type IDs)
4. **Register tracking**: если `cmp eax, ecx` — ищем назад `mov ecx, 0x8E`
5. **Remap table supplement**: после dispatch chain дополняем типами из remap table (group threshold <= 5)
6. **Dynamic type discovery**: если при парсинге CHEAT_CHECKS_REQUEST встречается неизвестный тип, `TryAssignSize` пробует назначить размер на лету с системой квот

**Оптимизации remap-извлечения**:
1. **Handler filtering**: вычисляем `maxHandler = (remapOff - jtableOff) / 4 - 1` из размера jump table, фильтруем записи с невалидным handler index
2. **Best single remap**: перебираем ВСЕ remap таблицы, выбираем ту, которая даёт результат ближе всего к 9 entries
3. **FixMaxType backward scan**: для таблиц с `maxType=0xFF` — backward scan от `movzx byte [reg+remapOff]` до последней пары CMP+JA/JAE, извлекаем реальный upper bound
4. **Upper-bound validation**: принимаем remap supplement только при `remapTypes.size() <= 12`
5. **Remap group threshold**: <= 5 entries per handler (было <= 3, не хватало для remap-only модулей)

**Dynamic type discovery** (re-enabled 2026-02-16):
- `TryAssignSize`: попытка назначить размер неизвестному типу при первой встрече в check section
- **Quota system** (`kSizeMaxCount[]`): {1, 3, 1, 1, 1, 2, 1} для размеров {31, 29, 25, 24, 6, 1, 0} — предотвращает чрезмерное назначение (например, 4 PAGE по 29 байт)
- **Structural validation**: PROC проверяет modIdx/procIdx <= numStrings + addr validity; PAGE проверяет addr + readLen; DRIVER проверяет strIdx; MEM проверяет addr + readLen
- **2-step lookahead fallback**: если primary structural validation не срабатывает для всех размеров, пробуем каждый кандидат и проверяем 2 следующих type byte (должны быть известными)

**Результаты**:
- **Python** (`validate_all_modules.py`): 21/21 модулей — 100% (9-10 типов каждый)
- **C++ (offline, RLE-unpacked binary)**: 21/21 модулей (14 dispatch chain, 6 remap-only, 1 partial+supplement)
- **C++ (live, in-memory)**: 0 unknown types, 0 PARTIAL parses — все пакеты с 10 чеками разобраны полностью
- **C++ (live, new modules)**: модули 398550DE, 90278079, E191991E и др. — корректно обработаны через remap + dynamic discovery

**Статус**: ПОЛНОСТЬЮ РАБОТАЕТ (24+ модулей, 0 unknown types в живых тестах)

---

### 6. Нахождение модуля в памяти

**Описание**: поиск загруженного Warden модуля в памяти процесса.

**Проблема**: модуль загружается через VirtualAlloc (не PE), адрес меняется при каждой загрузке.

**Решение**: стабильная сигнатура (26 байт)

**Сигнатура**:
```
56 57 FC 8B 54 24 14 8B 74 24 10 8B 44 24 0C 8B CA 8B F8 C1 E9 02 74 02 F3 A5
```

Это memcpy-подобная функция, присутствует во всех известных модулях.

**Статус**: ПОЛНОСТЬЮ РАБОТАЕТ

---

### 7. Shadow Copy (чтение оригинальных байт .text)

**Описание**: PE mapping WoW.exe с диска для получения неизменённых (unhook) байт кода.

**Проблема**: наши хуки модифицируют .text секцию — Warden MEM_CHECK видит изменения.

**Решение**: читаем оригинальные байты из файла на диске (не из памяти процесса).

**Алгоритм**:
1. Открываем Wow.exe с диска (GetModuleFileNameW)
2. Создаём file mapping (CreateFileMappingW)
3. Маппим в память (MapViewOfFile)
4. Парсим PE header (IMAGE_DOS_HEADER, IMAGE_NT_HEADERS32)
5. Находим .text секцию (IMAGE_SECTION_HEADER)
6. Конвертируем runtime address в file offset
7. Читаем байты из mapped view

**Применение**: активно используется спуфером `warden_spoof.cpp` для подмены MEM_CHECK результатов.

**Статус**: ПОЛНОСТЬЮ РАБОТАЕТ (используется для spoofing)

---

### 8. Детерминированный парсер (определение размеров типов)

**Описание**: автоматическое определение длины данных для каждого типа проверки при первой встрече.

**Проблема**: CHEAT_CHECKS_REQUEST содержит массив проверок разных типов, без разделителей.

**Решение**: Детерминированный парсер с структурной валидацией + dynamic type discovery с квотами.

**Фиксированные размеры** (confirmed from AzerothCore source):
- TIMING=0, MPQ=1, LUA=1, MEM=6, MODULE=24, DRIVER=25, PAGE_A=29, PAGE_B=29, PROC=31
- MEM: unk(1)+addr(4)+readLen(1)
- MODULE: seed(4)+SHA1(20)
- DRIVER: seed(4)+SHA1(20)+strIdx(1)
- PAGE: seed(4)+SHA1(20)+addr(4)+readLen(1)
- PROC: seed(4)+SHA1(20)+modIdx(1)+procIdx(1)+addr(4)+readLen(1)

**Валидация readLen**: `readLen <= 64` (было 40 — некоторые PAGE_CHECK используют readLen=48)

**Dynamic type discovery**: если при парсинге встречается тип, которого нет в результатах статического сканирования, `TryAssignSize` пытается назначить размер с помощью structural validation + 2-step lookahead. Quota system предотвращает лавинообразное назначение одного размера (например, PAGE=29).

**Статус**: ПОЛНОСТЬЮ РАБОТАЕТ (0 ошибок парсинга, 0 PARTIAL в живых тестах)

---

### 9. ScanForHookAddresses (детектирование проверок наших хуков)

**Описание**: module-agnostic поиск адресов наших хуков в сырых байтах CHEAT_CHECKS_REQUEST.

**Статус**: ПОЛНОСТЬЮ РАБОТАЕТ

---

### 10. RC4 CMSG расшифровка через внутренний хук

**Описание**: перехват RC4 PRGA функций ВНУТРИ бинарного модуля Warden для захвата plaintext CMSG ответов.

**Решение**: хук на RC4 PRGA функции внутри модуля (warden_rc4_hook.cpp). Некоторые модули используют **разные RC4 функции** для main thread и module thread, поэтому хукаем **все** найденные функции (до 4 одновременно).

**Multi-hook архитектура**:
- 4 отдельных naked stub'а (`HookedRC4Naked_0` — `HookedRC4Naked_3`), каждый с собственным trampoline
- Общий `RC4DetourHandler` для всех слотов
- Auto-detection calling convention (6 вариантов: ECX/EDX/EAX/EBX/ESI/EDI, обычный/swapped порядок)
- ConsumePlaintext: one-shot retrieval (thread-safe через CRITICAL_SECTION)

**Lifecycle**:
- **Early install**: в `WardenPreHandler`, ДО обработки handler'а — для перехвата HASH_RESULT (отправляется синхронно во время SMSG handler'а)
  - `FindModuleInMemory(nullptr, 0)` ищет модуль по стабильной 26-byte сигнатуре (не требует decompressed binary)
  - Устанавливается однократно (guard `!IsActive()`)
- **Late install**: в MODULE_INITIALIZE (guard: `!IsActive()`) — если early install не сработал
- Removed: на MODULE_USE и при Shutdown — все хуки снимаются
- Fallback: если ни одна RC4 функция не найдена — используем S-box cloning (warden_rc4.cpp)

**Статус**: ПОЛНОСТЬЮ РАБОТАЕТ (100% success rate — все CMSG расшифрованы через [rc4_hook])

---

### 11. Checksum и request-response корреляция

**Описание**: алгоритм checksum и structured parsing CMSG CHEAT_CHECKS_RESULT.

**Checksum algorithm**: SHA1(results) -> 5 x uint32_t LE -> XOR-fold -> uint32_t
- Реализовано в `warden_checksum.cpp` через WinCrypt CALG_SHA1
- Все наблюдаемые checksums валидируются корректно

**Request-response correlation**: FIFO queue (`std::deque<vector<PendingCheck>>`)
- Warden отправляет несколько SMSG request'ов до получения CMSG response'ов
- Каждый request пушится в очередь (даже empty/truncated — для синхронизации)
- При получении CMSG — pop из очереди для корреляции
- Queue очищается на MODULE_USE

**Per-check result parsing** (confirmed по категориям):
- **TIMING**: 5 bytes always (flag:1 + ticks:4)
- **MEM_CHECK**: 1 byte if fail / 1+readLen bytes if OK
- **LUA_EVAL**: 1 byte if fail / 1+1+strlen bytes if OK
- **PAGE/PROC/MODULE/DRIVER**: 1 byte (0xE9=pass)
- **MPQ_CHECK**: 1 byte if fail / 1+20 bytes if OK

**Статус**: ПОЛНОСТЬЮ РАБОТАЕТ

---

### 12. MEM_CHECK / PAGE_CHECK Spoofing (Variant A)

**Описание**: подмена результатов MEM_CHECK и PAGE_CHECK в CMSG plaintext ДО RC4 шифрования, чтобы сервер не видел наши inline hooks.

**Проблема**: MinHook перезаписывает 5+ байт JMP-инструкцией на адресах хуков. Если MEM_CHECK попадает на эти адреса — сервер видит JMP-патч вместо оригинального пролога.

**Подход**: **Variant A** — модификация plaintext CMSG в RC4 hook, перед шифрованием.

**Почему Variant A, а не Variant D (disable hooks)**:
- Variant D провалился: Warden модуль обрабатывает проверки **асинхронно** на отдельном потоке (17-28 секунд задержка после SMSG)
- Хуки отключались на ~1мс во время SMSG handler'а, но модуль читал память 20+ секунд позже
- Variant A не имеет этой проблемы: подменяем данные в момент шифрования

**Реализация** (файлы: `warden_spoof.h`, `warden_spoof.cpp`):

1. **FIFO queue** (`PushPendingChecks` / `PopPendingChecks`): хранит вектора PendingCheck с полями `checkAddr` и `readLen` для MEM/PAGE проверок
2. **SpoofCmsgIfNeeded()**: вызывается из `RC4DetourHandler` в `warden_rc4_hook.cpp` ДО шифрования
   - Copy-based rebuild: читает старые результаты, пишет новые, модифицируя при необходимости
   - Для MEM_CHECK: читает оригинальные байты из **shadow_copy** (`shadow::GetCleanBytes`), заменяет в CMSG
   - Для PAGE_CHECK: форсирует result byte = 0xE9 (pass)
   - Для MODULE_CHECK: форсирует result byte = 0xE9 (backup к PEB unlinking)
   - Для LUA_EVAL: селективная подмена addon-detection результатов
   - **Partial checks**: если pending checks список неполный (parser не распознал все типы), оставшиеся байты результатов копируются as-is — предотвращает truncation
   - Пересчитывает checksum (`warden_checksum::BuildChecksum`)

**Защищённые адреса** (kHookTargets):
| Адрес | Функция | Patch size |
|-------|---------|-----------|
| 0x00819210 | FrameScript_Execute | 8 bytes |
| 0x007DA850 | WardenHandler | 8 bytes |
| 0x00632B50 | SendPacket | 8 bytes |
| 0x00774EA0 | ARC4::Process | 8 bytes |

**Статус**: ПОЛНОСТЬЮ РАБОТАЕТ (в живых тестах — все checksums VALID, нет дисконнектов)

---

### 13. LUA_EVAL Spoofing (селективный)

**Описание**: подмена LUA_EVAL результатов в CMSG для сокрытия запрещённых аддонов.

**Проблема**: Warden отправляет LUA_EVAL проверки вида `return GetAddOnInfo("CheatAddon")`. Если аддон установлен — клиент возвращает его имя → бан.

**Подход**: **Селективный** — спуфим только результаты LUA_EVAL, чей eval code содержит addon-detection API:
- `GetAddOnInfo`, `IsAddOnLoaded`, `GetAddOnMetadata`
- `GetAddOnEnableState`, `GetAddOnDependencies`, `GetAddOnOptionalDependencies`

Все остальные LUA результаты (realm info, CVars, etc.) проходят без изменений.

**Реализация** (файл: `warden_spoof.cpp`):
1. `ShouldSpoofLuaResult(evalCode)` — substring match по addon-detection паттернам
2. В LUA case `SpoofCmsgIfNeeded`: если eval code match + результат непустой → заменяем на empty string
3. Если eval code не match или результат уже пустой → copy as-is

**Статус**: ПОЛНОСТЬЮ РАБОТАЕТ

---

### 14. MODULE_CHECK Evasion (PEB Unlinking + Response Spoofing)

**Описание**: двухуровневая защита от обнаружения нашей DLL через Warden MODULE_CHECK.

**Проблема**: Warden MODULE_CHECK перечисляет загруженные модули через `CreateToolhelp32Snapshot(TH32CS_SNAPMODULE)` → `Module32First/Module32Next`, которые читают PEB.Ldr lists. Наша DLL (`wotlk.dll`) и её зависимости (`glog.dll`, `gflags_debug.dll`) видны в этих списках.

**Layer 1: PEB Unlinking** (превентивная, файл: `peb_unlink.h`, `peb_unlink.cpp`):
- Удаляем нашу DLL + зависимости из всех 3 PEB.Ldr doubly-linked lists:
  - `InLoadOrderModuleList`
  - `InMemoryOrderModuleList`
  - `InInitializationOrderModuleList`
- После unlinking `CreateToolhelp32Snapshot` не видит эти модули
- `UnlinkAll(hModule)` вызывается ПОСЛЕ всей инициализации (hooks, shadow copy)
- `RelinkAll()` вызывается ПЕРЕД eject (нужен для `LdrUnloadDll`)

**Скрытые модули**:
| DLL | Причина |
|-----|---------|
| `wotlk.dll` | Наша DLL (primary target) |
| `glog.dll` | vcpkg x86 динамическая зависимость |
| `gflags_debug.dll` / `gflags.dll` | glog зависимость |

MinHook = статическая линковка (нет DLL). miniz = компилируется из исходников. CRT DLL = системные (WoW их сам загружает).

**Layer 2: Response Spoofing** (backup, в `warden_spoof.cpp`):
- Если MODULE_CHECK result != 0xE9 → форсируем 0xE9 (pass)
- Работает как backup на случай, если PEB unlinking не сработает

**Статус**: ПОЛНОСТЬЮ РАБОТАЕТ (PEB unlinking подтверждён в живых тестах, MODULE_CHECK не обнаруживает DLL)

---

### 15. HASH_REQUEST / HASH_RESULT перехват

**Описание**: полный перехват цикла HASH_REQUEST (SMSG opcode 0x05) → HASH_RESULT (CMSG opcode 0x04).

**Проблема**: HASH_RESULT отправляется клиентом **синхронно** во время SMSG handler'а (ДО MODULE_INITIALIZE). Ранее RC4 хуки ставились только в MODULE_INITIALIZE — HASH_RESULT не перехватывался.

**Решение**: **Early RC4 hook install** в `WardenPreHandler`:
1. Перед вызовом оригинального handler'а проверяем: `!warden_rc4_hook::IsActive()`
2. Если модуль ещё не найден: `FindModuleInMemory(nullptr, 0)` — использует стабильную 26-byte сигнатуру, НЕ требует decompressed binary
3. Если модуль найден: устанавливаем RC4 хуки → перехватываем HASH_RESULT при шифровании

**Парсинг HASH_REQUEST** (в `WardenPostHandlerImpl`):
- Извлекаем 16-byte seed (bytes 1..16)
- Сохраняем через `warden_spoof::StoreHashSeed()` для сопоставления с HASH_RESULT

**Парсинг HASH_RESULT** (в `SendPacketHandler` + `RC4DetourHandler`):
- RC4 hook захватывает CMSG plaintext (21 байт: [0x04][SHA1:20])
- `SpoofHashResultIfNeeded()` — инфраструктура для будущей подмены (текущая реализация: no-op stub, логирует SHA1)
- SendPacket логирует HASH_RESULT + сопоставляет с сохранённым seed

**Исправленные опкоды CMSG** (warden_types.h):
| Опкод | Значение | Было |
|-------|----------|------|
| MEM_CHECKS_RESULT | 0x03 | 0x04 (неверно) |
| HASH_RESULT | 0x04 | 0x05 (неверно) |
| MODULE_FAILED | 0x05 | отсутствовал |

**Статус**: ПОЛНОСТЬЮ РАБОТАЕТ (HASH_RESULT перехватывается, RC4 хуки ставятся ДО handler'а)

---

## Что НЕ работает (TODO)

### 1. DRIVER_CHECK / MPQ_CHECK / PROC_CHECK spoofing

**Описание**: подмена результатов этих проверок.

**Текущее состояние**: все три типа корректно парсятся и логируются. Результаты проходят без модификации (pass-through).

**Приоритет**: НИЗКИЙ (эти проверки не нацелены на наш DLL)

**Статус**: НЕ НАЧАТО (pass-through, парсинг работает)

---

## Архитектура файлов

```
wotlk/
  dllmain.cpp                          — точка входа, MainThread, init/shutdown sequence
  hooks/
    hooks.h                            — API: Initialize() / Shutdown()
    hooks.cpp                          — MinHook + FrameScript/Warden/SendPacket/ARC4 detours
                                         + CHEAT_CHECKS_REQUEST parser + CMSG parser
                                         + early RC4 install + HASH_REQUEST/RESULT parsing
  warden/
    warden_types.h                     — CheckCategory enum, PendingCheck struct, CMSG/SMSG opcodes
    warden_scan.h / .cpp               — type extraction (dispatch chain, remap, dynamic discovery)
    warden_spoof.h / .cpp              — CMSG spoofing (MEM/PAGE/MODULE/LUA/HASH), FIFO queue, hash seed storage
    warden_rc4_hook.h / .cpp           — RC4 PRGA hook inside module (multi-hook, up to 4) + early install
    warden_rc4.h / .cpp                — RC4 S-box cloning fallback
    warden_checksum.h / .cpp           — SHA1 XOR-fold checksum
    shadow_copy.h / .cpp               — PE mapping Wow.exe для чистых байт
    module_dump.h / .cpp               — module capture + disk cache
    peb_unlink.h / .cpp                — PEB.Ldr unlinking for MODULE_CHECK evasion
  logging/
    glog_custom_formatter.hpp / .cpp   — custom glog sink с цветным выводом
    logger_setup.hpp / .cpp            — glog initialization
  third_party/
    miniz.h / .c + miniz_*.h / .c      — zlib decompression
```

---

## Roadmap

### Выполнено

- ~~Инжекция/выгрузка DLL~~ — полный цикл с safe self-unload
- ~~Перехват FrameScript_Execute~~ — мониторинг Lua, фильтрация UI скриптов
- ~~Перехват SMSG_WARDEN_DATA~~ — return-address hijack, парсинг всех типов
- ~~Захват модулей Warden~~ — 24 модуля, disk cache, zlib decompression
- ~~Извлечение типов из модуля~~ — dispatch chain + remap + dynamic discovery (0 unknown)
- ~~Нахождение модуля в памяти~~ — stable 26-byte signature
- ~~Shadow Copy~~ — PE mapping Wow.exe для чистых байт
- ~~Детерминированный парсер~~ — structural validation + quota system
- ~~RC4 CMSG расшифровка~~ — internal hook (multi-hook, 100% success rate)
- ~~Checksum + request-response correlation~~ — SHA1 XOR-fold, FIFO queue
- ~~MEM_CHECK spoofing~~ — Variant A (shadow copy replacement)
- ~~PAGE_CHECK spoofing~~ — Variant A (force 0xE9 pass)
- ~~LUA_EVAL spoofing~~ — селективный (addon-detection patterns)
- ~~MODULE_CHECK evasion~~ — PEB unlinking + response spoofing backup
- ~~HASH_REQUEST/RESULT перехват~~ — early RC4 install + seed/hash parsing + spoof infrastructure

### Ближайшее

#### 1. Длительный боевой тест
**Цель**: многочасовая сессия без дисконнектов.
**Критерий успеха**: 0 дисконнектов при активных хуках, все CMSG checksums VALID, MODULE_CHECK не обнаруживает DLL.

#### 2. Статистика проверок (dashboard)
**Цель**: знать заранее, какие адреса Warden проверяет.
**Механизм**: собираем частоты MEM_CHECK/PAGE_CHECK адресов, ALERT если hook-адрес проверяется.

### Среднесрочное

#### 3. Полный Warden bypass

| Компонент | Статус |
|-----------|--------|
| MEM_CHECK spoofing | РАБОТАЕТ (Variant A) |
| PAGE_CHECK spoofing | РАБОТАЕТ (Variant A) |
| LUA_EVAL spoofing | РАБОТАЕТ (селективный) |
| MODULE_CHECK evasion | РАБОТАЕТ (PEB unlink + response spoof) |
| HASH_REQUEST/RESULT перехват | РАБОТАЕТ (early RC4 install + spoof stub) |
| DRIVER_CHECK spoofing | НЕ НАЧАТО (pass-through) |
| MPQ_CHECK spoofing | НЕ НАЧАТО (pass-through) |
| PROC_CHECK spoofing | НЕ НАЧАТО (pass-through) |

---

## Метрики успеха

### Текущий прогресс
- **Наблюдение**: 100% — все типы пакетов парсятся, CMSG расшифровывается
- **Spoofing**: MEM_CHECK + PAGE_CHECK + MODULE_CHECK + LUA_EVAL — РАБОТАЕТ в живых тестах
- **Критические блокеры**: 0
- **Полнота анализа**: 24 модуля захвачено (21 decompressed), 21/21 = 100% через Python, 0 unknown в живых C++ тестах
- **Извлечение типов**: dispatch chain (14) + remap-only (6) + partial+supplement (1) = 21/21 offline; remap + dynamic discovery для live модулей
- **RC4 CMSG**: расшифровка через internal hook — 100% success rate
- **Checksum**: SHA1 XOR-fold — все наблюдаемые checksums VALID
- **Request-response correlation**: FIFO queue — РАБОТАЕТ (delta=0)
- **PEB Unlinking**: DLL + зависимости скрыты из PEB.Ldr lists

---

## Заключение

Проект находится на стадии **активного bypass**. Все основные типы проверок Warden, нацеленные на detection нашей DLL, перехвачены и спуфятся:

**Что мы умеем**:
- Перехватывать и парсить все типы Warden пакетов (SMSG и CMSG)
- Извлекать бинарные модули и их внутреннюю структуру (24 модуля захвачено)
- Автоматически определять типы проверок из любого модуля (dispatch chain + remap + dynamic discovery, 0 unknown types)
- Расшифровывать CMSG ответы через internal RC4 hook (multi-hook, до 4 функций)
- Коррелировать SMSG requests с CMSG responses (FIFO queue)
- Валидировать и пересчитывать checksums (SHA1 XOR-fold)
- **Подменять MEM_CHECK / PAGE_CHECK результаты** на оригинальные байты из shadow copy
- **Подменять MODULE_CHECK результаты** → forced pass (backup к PEB unlinking)
- **Подменять LUA_EVAL результаты** addon-detection запросов (селективный spoof)
- **Скрывать DLL из PEB.Ldr** — MODULE_CHECK не видит wotlk.dll / glog.dll / gflags*.dll
- **Перехватывать HASH_REQUEST/RESULT** — early RC4 install + seed storage + spoof infrastructure
- Находить адреса наших хуков в запросах Warden (ScanForHookAddresses)
- Читать оригинальные байты .text секции (Shadow Copy)
- Обрабатывать новые (uncached) модули через blind memory scan + dynamic type discovery

**Ключевые компоненты**:
- `warden_spoof.cpp` — core spoofing logic + FIFO queue
- `warden_rc4_hook.cpp` — вызывает SpoofCmsgIfNeeded перед RC4 encrypt
- `warden_scan.cpp` — dispatch chain + remap + dynamic discovery
- `shadow_copy.cpp` — оригинальные байты из Wow.exe на диске
- `warden_checksum.cpp` — пересчёт checksum после модификации
- `peb_unlink.cpp` — скрытие DLL из PEB.Ldr lists

**Живые тесты** (2026-02-16):
- In-memory scan: 9-10 типов, 0 unknown, 0 PARTIAL, все checksums VALID
- RC4 hook: 100% CMSG captured [rc4_hook], early install перехватывает HASH_RESULT
- Spoofing: MEM/PAGE/MODULE/LUA работает, нет дисконнектов
- HASH_REQUEST/RESULT: seed сохраняется, SHA1 логируется, SpoofHashResultIfNeeded stub работает
- Queue correlation: delta=0, все результаты разобраны по категориям
- PEB Unlinking: DLL не видна в module enumeration
- Dynamic discovery: новые remap-only модули обрабатываются корректно (398550DE, 90278079 и др.)

**Исправленные баги** (2026-02-16):
- **resultLen truncation**: при частичном pending checks (parser не распознал все типы) SpoofCmsgIfNeeded обрезала результаты — вызывало дисконнект. Исправлено: оставшиеся байты копируются as-is.
- **CMSG opcodes**: HASH_RESULT=0x04 (было 0x05), MEM_CHECKS_RESULT=0x03 (было 0x04), добавлен MODULE_FAILED=0x05.
