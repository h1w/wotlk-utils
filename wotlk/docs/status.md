# Статус проекта wotlk-utils

Этот документ показывает текущее состояние проекта: что уже работает, что не работает, и к чему мы стремимся.

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
- Это ускоряет повторные подключения и позволяет собирать модули, которые больше не скачиваются

**Формат модуля** (НЕ PE!):
- 40-byte header: moduleSize, relocOff, relocCount, exportTableOff, exportCount, baseIndex, importTableOff, importLibCount, sectionDescCount
- Section descriptors: 12 байт на секцию (RLE-packed)
- Relocation table: delta-encoded
- In-memory: VirtualAlloc с PAGE_EXECUTE_READWRITE, MEM_PRIVATE

**Известные модули** (24 модуля захвачено, 21 с decompressed бинарниками):
- **7C4ABC97**: decompressed=29234 bytes (REFERENCE module)
- **9A95D199**: decompressed=28876 bytes
- **CB9E43D6**: decompressed=31718 bytes, runtimeSize=49152
- **0BE6B21C**: decompressed=30132 bytes, runtimeSize=45056 (RC4 uses EAX register)
- **952860B1**: decompressed=26065 bytes, runtimeSize=40960
- **32F1D632**: 10 types via union of 2 dispatch chains
- **46DCC0B8**: decompressed=33057 bytes, runtimeSize=49152 (remap-only)
- **026D6F30**, **03730574**, **0AC0C559**, **1A615549**, **232577E7**, **2A3CAB97**, **2A7BD368**, **3E02C87E**, **3FEB1D16**, **6FAE26D6**, **7281AE6C**, **85F90209**, **BD38DD63**, **E348326F**: dispatch chain / remap
- **2C045995**, **4109957D**, **CDD39A8A**: только encrypted/decrypted (без decompressed)

**Статус**: ПОЛНОСТЬЮ РАБОТАЕТ

---

### 5. Извлечение типов проверок из модуля

**Описание**: определение module-specific check type IDs из бинарного модуля.

**Проблема**: каждый модуль использует свои ID для типов проверок (не совпадают с TC константами).

**Решение**: In-memory scan (primary) + RLE-unpacked binary scan (fallback) + XOR-anchored dispatch chain + remap table supplement

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
5. **Remap table supplement**: после dispatch chain дополняем типами из remap table

**Оптимизации remap-извлечения** (портированы из Python `validate_all_modules.py`, 2026-02-16):
1. **Handler filtering**: вычисляем `maxHandler = (remapOff - jtableOff) / 4 - 1` из размера jump table, фильтруем записи с невалидным handler index в `ExtractFromRemapCrossRef` и `ExtractFromSingleRemap`
2. **Best single remap**: перебираем ВСЕ remap таблицы, выбираем ту, которая даёт результат ближе всего к 9 entries (вместо break на первой успешной)
3. **FixMaxType backward scan**: для таблиц с `maxType=0xFF` (CMP < 0x80 не найден стандартным сканером) — backward scan от `movzx byte [reg+remapOff]` до последней пары CMP+JA/JAE, извлекаем реальный upper bound
4. **Upper-bound validation**: принимаем remap supplement только при `remapTypes.size() <= 12` (защита от мусора в Step 3)

**Исправления** (2026-02-16):
- **readLen cap**: увеличен с 40 до 64 в `TryAssignSize` — некоторые PAGE_CHECK имеют readLen=48
- **g_scanBaseAddr в blind scan**: устанавливается `= baseAddr` перед `ScanForDispatchChainInBinary` в `ScanRegionForDispatcher`, сбрасывается в 0 при неудаче — фикс абсолютных displacement'ов remap таблиц в in-memory модулях

**Результаты**:
- **Python** (`validate_all_modules.py`): 21/21 модулей — 100% (9-10 типов каждый)
- **C++ (offline, RLE-unpacked binary)**: протестировано 21 модуль
  - **14/21 FULL**: 9+ типов из dispatch chains
  - **1/21 PARTIAL**: dispatch chain + remap supplement → 9 типов
  - **6/21 REMAP ONLY**: handler filtering + best single remap + FixMaxType → 9-10 типов
- **C++ (live, in-memory scan)**: 0 unknown types в живых тестах
- **C++ (live, blind scan)**: корректно обрабатывает новые (uncached) модули с абсолютными displacement'ами

**Статус**: ПОЛНОСТЬЮ РАБОТАЕТ (21+ модуль, 0 unknown types в живых тестах)

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

**Решение**: Детерминированный парсер с структурной валидацией (НЕ brute-force).

**Фиксированные размеры** (confirmed from AzerothCore source):
- TIMING=0, MPQ=1, LUA=1, MEM=6, MODULE=24, DRIVER=25, PAGE_A=29, PAGE_B=29, PROC=31
- MEM: unk(1)+addr(4)+readLen(1)
- MODULE: seed(4)+SHA1(20)
- DRIVER: seed(4)+SHA1(20)+strIdx(1)
- PAGE: seed(4)+SHA1(20)+addr(4)+readLen(1)
- PROC: seed(4)+SHA1(20)+modIdx(1)+procIdx(1)+addr(4)+readLen(1)

**Валидация readLen**: `readLen <= 64` (было 40 — некоторые PAGE_CHECK используют readLen=48)

**Статус**: ПОЛНОСТЬЮ РАБОТАЕТ (0 ошибок парсинга при корректном извлечении типов из модуля)

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
- Auto-detection calling convention (6 вариантов: ECX/EDX/EAX/stack-based, обычный/swapped порядок)
- ConsumePlaintext: one-shot retrieval (thread-safe через CRITICAL_SECTION)

**Lifecycle**:
- Installed: после MODULE_INITIALIZE (FindModuleInMemory) — до 4 хуков одновременно
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
- Variant D был реализован и протестирован первым
- Variant D провалился: Warden модуль обрабатывает проверки **асинхронно** на отдельном потоке (17-28 секунд задержка после SMSG)
- Хуки отключались на ~1мс во время SMSG handler'а, но модуль читал память 20+ секунд позже, когда хуки уже обратно включены
- Variant A не имеет этой проблемы: подменяем данные в момент шифрования, когда ответ уже сформирован

**Реализация** (файлы: `warden_spoof.h`, `warden_spoof.cpp`):

1. **FIFO queue** (`PushPendingChecks` / `PopPendingChecks`): хранит вектора PendingCheck с полями `checkAddr` и `readLen` для MEM/PAGE проверок
2. **SpoofCmsgIfNeeded()**: вызывается из `RC4DetourHandler` в `warden_rc4_hook.cpp` ДО шифрования
   - Peek front очереди (не pop — pop делает ParseCheatChecksResult позже)
   - Ходит по results секции CMSG в порядке checks
   - Для каждого MEM_CHECK с result=0x00 (success), чей адрес пересекается с нашими хуками:
     - Читает оригинальные байты из **shadow_copy** (`shadow::GetCleanBytes`)
     - Заменяет данные в CMSG на чистые байты
   - Для каждого PAGE_CHECK, чей адрес пересекается с нашими хуками:
     - Форсирует result byte = 0xE9 (pass)
   - Если были изменения — пересчитывает checksum (`warden_checksum::BuildChecksum`)
3. **Write-back**: если spoofer модифицировал буфер — `SafeReadBytes` записывает обратно в буфер модуля перед RC4

**Защищённые адреса** (kHookTargets):
| Адрес | Функция | Patch size |
|-------|---------|-----------|
| 0x00819210 | FrameScript_Execute | 8 bytes |
| 0x007DA850 | WardenHandler | 8 bytes |
| 0x00632B50 | SendPacket | 8 bytes |
| 0x00774EA0 | ARC4::Process | 8 bytes |

**Thread safety**: peek (SpoofCmsgIfNeeded) и pop (ParseCheatChecksResult) происходят последовательно на одном потоке модуля. Main thread пушит только в BACK очереди.

**Тестирование** (2026-02-15):
- Сборка: OK (Debug|Win32)
- Живой тест: все checksums VALID, RC4 hook захватывает 100% CMSG, queue depth корректен
- Спуфер **пока не срабатывал** — ни один MEM_CHECK за сессию не попал на наши адреса
- Логика result walker'а идентична протестированному ParseCheatChecksResult

**Статус**: РЕАЛИЗОВАНО, ожидает боевого теста (MEM_CHECK на наш адрес)

---

## Что НЕ работает (TODO)

### 1. Подмена LUA_EVAL результатов

**Описание**: модификация LUA_EVAL ответов в CMSG для сокрытия запрещённых аддонов.

**Пример LUA_EVAL**:
```lua
return GetAddOnInfo("SomeCheat")
```

Если аддон установлен — клиент вернёт его имя — возможен бан.

**Механизм**:
1. Перехватываем LUA_EVAL в CHEAT_CHECKS_REQUEST (уже есть hook на FrameScript_Execute)
2. В SpoofCmsgIfNeeded: находим LUA result в CMSG, подменяем на безопасный результат
3. Пересчитываем checksum и resultLen

**Приоритет**: СРЕДНИЙ

**Статус**: НЕ НАЧАТО

---

### 2. Антиопределение DLL (MODULE_CHECK evasion)

**Описание**: скрыть нашу DLL от Warden MODULE_CHECK.

**Проблема**: Warden может вызывать NtQueryVirtualMemory — видит все загруженные модули.

**Решение** (варианты):
- Manual mapping: загружаем DLL без LoadLibrary (не попадает в PEB.Ldr lists)
- Hook на NtQueryVirtualMemory: скрываем наши регионы из результатов

**Приоритет**: НИЗКИЙ (пока MODULE_CHECK не целится на нашу DLL)

**Статус**: НЕ НАЧАТО

---

### 3. HASH_REQUEST spoofing

**Описание**: подмена ответа на HASH_REQUEST (opcode 0x05).

**Текущее состояние**: HASH_REQUEST получается и логируется, HASH_RESULT (21 bytes) отправляется клиентом. Алгоритм вычисления хеша — внутри модуля, пока не реверсирован.

**Приоритет**: НИЗКИЙ (HASH_REQUEST не связан с detection наших хуков напрямую)

**Статус**: НЕ НАЧАТО

---

## Roadmap

### Выполнено

- ~~Разбор формата CHEAT_CHECKS_RESULT~~ — request-response correlation, per-check parsing
- ~~Алгоритм checksum~~ — SHA1 XOR-fold, `warden_checksum.cpp`
- ~~Парсинг CMSG ответов~~ — structured parsing с correlation
- ~~MEM_CHECK spoofing через Shadow Copy~~ — **Variant A** реализован (`warden_spoof.cpp`)
- ~~PAGE_CHECK spoofing~~ — покрыт Variant A (force 0xE9 pass)

### Ближайшее

#### 1. Боевой тест MEM_CHECK spoofing
**Цель**: дождаться MEM_CHECK на один из наших 4 hook-адресов и убедиться, что спуфер сработает корректно.

**Критерий успеха**: в логе появится `[SPOOF] MEM_CHECK ... replaced with clean bytes`, checksum VALID, нет дисконнекта.

#### 2. Подмена LUA_EVAL результатов
**Цель**: скрыть запрещённые аддоны / модификации UI.

**Механизм**: расширить SpoofCmsgIfNeeded для обработки LUA category.

### Среднесрочное (1-2 месяца)

#### 3. Автоматическое определение опасности (dashboard)
**Цель**: знать заранее, какие адреса Warden проверяет.

**Механизм**:
1. Собираем статистику проверок (из всех CHEAT_CHECKS_REQUEST)
2. Если адрес нашего хука появляется в MEM_CHECK — ALERT
3. Считаем частоту: сколько раз в час Warden проверяет этот адрес

#### 4. Антиопределение DLL
**Цель**: скрыть нашу DLL от Warden MODULE_CHECK.

### Дальнее (3+ месяца)

#### 5. Полный Warden bypass

| Компонент | Статус |
|-----------|--------|
| MEM_CHECK spoofing | РЕАЛИЗОВАНО (Variant A) |
| PAGE_CHECK spoofing | РЕАЛИЗОВАНО (Variant A) |
| LUA_EVAL spoofing | НЕ НАЧАТО |
| HASH_REQUEST spoofing | НЕ НАЧАТО |
| MODULE_CHECK evasion | НЕ НАЧАТО |
| DRIVER_CHECK spoofing | НЕ НАЧАТО |
| MPQ_CHECK spoofing | НЕ НАЧАТО |
| PROC_CHECK spoofing | НЕ НАЧАТО |

---

## Метрики успеха

### Текущий прогресс
- **Наблюдение**: 100% — все типы пакетов парсятся, CMSG расшифровывается
- **Spoofing**: MEM_CHECK + PAGE_CHECK реализован (Variant A), ожидает боевого теста
- **Критические блокеры**: 0
- **Полнота анализа**: 24 модуля захвачено (21 decompressed), 21/21 = 100% через Python, 0 unknown types в живых C++ тестах
- **Извлечение типов**: dispatch chain (14 модулей) + remap-only (6 модулей) + partial+supplement (1 модуль) = 21/21
- **RC4 CMSG**: расшифровка через internal hook — 100% success rate
- **Checksum**: SHA1 XOR-fold — все наблюдаемые checksums VALID
- **Request-response correlation**: FIFO queue — РАБОТАЕТ

### Следующий milestone
- Боевой тест MEM_CHECK spoofing (дождаться MEM_CHECK на hook-адрес)
- После: LUA_EVAL spoofing

---

## Заключение

Проект находится на стадии **активного spoofing**. Наблюдательная фаза завершена, первый spoofer (MEM_CHECK / PAGE_CHECK) реализован.

**Что мы умеем**:
- Перехватывать и парсить все типы Warden пакетов (SMSG и CMSG)
- Извлекать бинарные модули и их внутреннюю структуру (24 модуля захвачено)
- Автоматически определять типы проверок из модуля (in-memory scan + dispatch chain + remap table + handler filtering + FixMaxType)
- Расшифровывать CMSG ответы через internal RC4 hook (multi-hook, до 4 функций)
- Коррелировать SMSG requests с CMSG responses (FIFO queue)
- Валидировать и пересчитывать checksums (SHA1 XOR-fold)
- **Подменять MEM_CHECK / PAGE_CHECK результаты** на оригинальные байты из shadow copy
- Находить адреса наших хуков в запросах Warden (ScanForHookAddresses)
- Читать оригинальные байты .text секции (Shadow Copy)
- Обрабатывать новые (uncached) модули через blind memory scan с корректным `g_scanBaseAddr`

**Ключевые компоненты Variant A spoofing**:
- `warden_spoof.cpp` — core spoofing logic + FIFO queue
- `warden_rc4_hook.cpp` — вызывает SpoofCmsgIfNeeded перед RC4 encrypt, пишет обратно в буфер модуля
- `shadow_copy.cpp` — предоставляет оригинальные байты из Wow.exe на диске
- `warden_checksum.cpp` — пересчёт checksum после модификации

**Живые тесты** (2026-02-16):
- In-memory scan: 9-10 типов, 0 unknown, все checksums VALID
- Blind memory scan: корректно обрабатывает uncached модули (g_scanBaseAddr fix)
- RC4 hook: 100% CMSG captured [rc4_hook]
- Spoofing: код готов, result walker идентичен доказанному ParseCheatChecksResult
- Queue correlation: delta=0, все результаты разобраны по категориям
- Python validation: 21/21 модулей = 100% (9-10 типов каждый)

Следующая фаза: **боевой тест spoofing** + расширение на LUA_EVAL.
