# Статус проекта wotlk-utils

Этот документ показывает текущее состояние проекта: что уже работает, что не работает, и к чему мы стремимся.

---

## Что уже работает (DONE ✓)

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

**Известные модули** (15+ модулей захвачено и проанализировано):
- **7C4ABC97**: decompressed=29234 bytes
- **DA3BF29E**: decompressed size varies
- **9A95D199**: decompressed=28876 bytes
- **CB9E43D692620E7B698C5CE085163E6E**: decompressed=31718 bytes, runtimeSize=49152
- **0BE6B21C**: decompressed=30132 bytes, runtimeSize=45056 (RC4 uses EAX register)
- **952860B1**: decompressed=26065 bytes, runtimeSize=40960
- **32F1D632**: 10 types via union of 2 dispatch chains
- **46DCC0B811CD2D7BB870A2F714CD2A61**: decompressed=33057 bytes, runtimeSize=49152 (remap-only)

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
4. **Blind memory scan** (`ScanAndExtractTypeIDs`): последний resort — все MEM_PRIVATE регионы

**Алгоритм извлечения типов**:
1. **Anchor search**: ищем паттерн `xor r8, [reg+4]` (опкод `32 [40-7F, rm≠4] 04`)
   - Это начало dispatcher'а (request parser)
   - XOR снимает xorByte, получаем реальный type ID
2. **movzx detection**: следующая инструкция `movzx eax, al` (расширяет type до 32-bit)
3. **Dispatch chain walker**: BFS queue-based обход дерева сравнений
   - **BFS с visited set**: max 16 branches, 400-byte scan limit, per-branch accumulator
   - **Union of ALL dispatch chains**: если модуль имеет несколько цепочек, объединяем типы
   - **cmp** (immediate): `cmp r32, imm8/imm32` → type ID прямо в opcode
   - **cmp** (register): `cmp r32, r32` → отслеживаем `mov r32, imm32` назад
   - **CMP values always inserted**: для greater/less family jumps (BST pivots are real type IDs)
   - **sub/dec + je/jne**: `sub r8, imm8` + `jne target` → обрабатываем вычитание, следуем по jne
4. **Register tracking**: если `cmp eax, ecx` → ищем назад `mov ecx, 0x8E` (пример)
5. **Remap table supplement**: после dispatch chain дополняем типами из remap table
   - `ExtractFromSingleRemap`: для одиночных таблиц — default handler = самое частое значение, остальные = type IDs
   - Remap cross-reference: пересечение singletons + pairs + triplets для множественных таблиц
   - Last-resort: chain extraction retried on remap-classified XOR sites

**Результаты**: Протестировано 15 модулей
- **11/15 FULL**: 9+ типов из dispatch chains (7C4ABC97, DA3BF29E, 9A95D199, 952860B1, 32F1D632, и др.)
- **1/15 PARTIAL**: 5 типов из dispatch chain + дополнение из remap
- **3/15 REMAP ONLY**: все типы из remap tables (ExtractFromSingleRemap)
- **In-memory scan**: 10/10 типов с первого раза, 0 unknown types в живых тестах

**Python скрипты** (в `wotlk/docs/scripts/`):
- `dispatch/comprehensive_analysis_v2.py`: улучшенный анализ (MOVZX window 18 bytes, BFS walker)
- `dispatch/remap_crossref_test.py`: тестирование remap cross-reference
- `dispatch/find_request_parsers.py`: находит dispatcher'ы через XOR-anchor (прорыв!)
- `dispatch/extract_all_types.py`: извлекает типы из всех модулей
- `dispatch/group_sizes.py`: группирует модули по размерам
- `verification/investigate_missing.py`: анализирует модули без dispatch chain
- `module_format/unpack_rle.py`: RLE-распаковщик модулей (Python, проверен на 10/10 дампах)

**Статус**: ПОЛНОСТЬЮ РАБОТАЕТ (15 модулей, 0 unknown types в живых тестах)

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

**Алгоритм**:
1. Перебираем MEM_PRIVATE регионы (VirtualQuery)
2. Сканируем каждый регион на наличие сигнатуры
3. Вычисляем runtime base address модуля

**Применение**: нужно для XOR-anchored scan (извлечение типов проверок в runtime)

**Статус**: ПОЛНОСТЬЮ РАБОТАЕТ

---

### 7. Shadow Copy (чтение оригинальных байт .text)

**Описание**: PE mapping WoW.exe с диска для получения неизменённых (unhook) байт кода.

**Проблема**: наши хуки модифицируют .text секцию → Warden MEM_CHECK видит изменения.

**Решение**: читаем оригинальные байты из файла на диске (не из памяти процесса).

**Алгоритм**:
1. Открываем Wow.exe с диска (GetModuleFileNameA)
2. Создаём file mapping (CreateFileMappingA)
3. Маппим в память (MapViewOfFile)
4. Парсим PE header (IMAGE_DOS_HEADER, IMAGE_NT_HEADERS)
5. Находим .text секцию (IMAGE_SECTION_HEADER)
6. Конвертируем RVA (Relative Virtual Address) в file offset
7. Читаем байты из mapped view

**Применение**: в будущем для подмены MEM_CHECK результатов (возвращать оригинальные хеши вместо хешей с хуками).

**Статус**: ПОЛНОСТЬЮ РАБОТАЕТ (но пока не используется для spoofing)

---

### 8. Детерминированный парсер (определение размеров типов)

**Описание**: автоматическое определение длины данных для каждого типа проверки при первой встрече.

**Проблема**: CHEAT_CHECKS_REQUEST содержит массив проверок разных типов, без разделителей. Мы не знаем, сколько байт занимает каждый тип.

**Решение**: Детерминированный парсер с структурной валидацией (НЕ brute-force).

**Алгоритм**:
1. Читаем check section (известной общей длины)
2. Для каждого check:
   - Читаем xor'd type byte
   - XOR с xorByte → реальный type ID
   - Если размер типа известен (в g_typeIDs) → пропускаем N байт
   - Если неизвестен → вызываем `TryAssignSize`
3. **TryAssignSize**: пробует кандидаты в порядке [31, 29, 25, 24, 6, 1, 0] (largest-first)
   - **Address validity**: `IsValidAddress(0x1000 - 0x7FFFFFFF)` для MEM/PAGE/PROC
   - **String index validation**: `strIdx <= numStrings` (relaxed, was `<`)
   - **SHA1 seed heuristic**: для MODULE/DRIVER/PAGE/PROC
   - **Look-ahead**: проверка, что остаток байт достаточен
   - Размер присваивается **на первом же успехе**, без backtracking
4. **Strict mode**: типы ДОЛЖНЫ быть извлечены из модуля до парсинга пакета
   - Если тип не в `g_typeIDs` → парсинг завершается с ошибкой
   - Никакого динамического обнаружения типов из пакетов — все типы из module scanning

**Фиксированные размеры** (confirmed from AzerothCore source):
- TIMING=0, MPQ=1, LUA=1, MEM=6, MODULE=24, DRIVER=25, PAGE_A=29, PAGE_B=29, PROC=31
- MEM: unk(1)+addr(4)+readLen(1)
- MODULE: seed(4)+SHA1(20)
- DRIVER: seed(4)+SHA1(20)+strIdx(1)
- PAGE: seed(4)+SHA1(20)+addr(4)+readLen(1)
- PROC: seed(4)+SHA1(20)+modIdx(1)+procIdx(1)+addr(4)+readLen(1)

**Пример**:
```
Check section: [0x8E] [unk][addr:4][len:1] [0x1F] [0x91] [strIdx]
               ^MEM   ^data (6 bytes)      ^TIMING  ^MPQ   ^data (1 byte)
```

При первой встрече типа: TryAssignSize(0x8E) → пробует 31,29,25,24,6 → валидация успешна на 6 → запомнили.

**Статус**: ПОЛНОСТЬЮ РАБОТАЕТ (0 ошибок парсинга при корректном извлечении типов из модуля)

---

### 9. ScanForHookAddresses (детектирование проверок наших хуков)

**Описание**: module-agnostic поиск адресов наших хуков в сырых байтах CHEAT_CHECKS_REQUEST.

**Проблема**: мы не всегда знаем типы проверок (DFS solver может fail на сложных пакетах).

**Решение**: сканируем весь check section на наличие 4-byte Little-Endian адресов из нашего списка хуков.

**Алгоритм**:
1. Список известных hook addresses (FrameScript_Execute, SendPacket, ARC4::Process, SMSG_WARDEN_DATA)
2. Сканируем check section:
   - Для каждого смещения читаем 4 байта как uint32_t (LE)
   - Проверяем: это один из наших адресов?
   - Если да → "ALERT! Found hook address 0x00819210 at offset X"

**Применение**: даже если DFS solver fail — мы видим, что Warden проверяет наши хуки.

**Статус**: ПОЛНОСТЬЮ РАБОТАЕТ

---

### 10. RC4 CMSG расшифровка через внутренний хук

**Описание**: перехват RC4 PRGA функций ВНУТРИ бинарного модуля Warden для захвата plaintext CMSG ответов.

**Проблема**: S-box cloning (warden_rc4.cpp) не работал — к моменту клонирования состояние уже менялось.

**Решение**: хук на RC4 PRGA функции внутри модуля (warden_rc4_hook.cpp). Некоторые модули используют **разные RC4 функции** для main thread и module thread, поэтому хукаем **все** найденные функции (до 4 одновременно).

**Алгоритм**:
1. **Pattern scanner** (`ScanRuntimeForAllRC4`): MOVZX/MOV cluster detection с disp32=0x100/0x101
   - Ищем инструкции вида `movzx r32, byte [reg+0x100]` (чтение i) и `movzx r32, byte [reg+0x101]` (чтение j)
   - RC4 context layout: `[S[256]][i][j]` — i смещён на 0x100, j на 0x101
2. **Multi-cluster detection**:
   - Группируем совпадения в кластеры (120-byte gap между соседними)
   - Отбираем кластеры с >= 2 совпадениями и обеими ссылками (0x100 + 0x101)
   - Для каждого кластера находим пролог функции (`FindFunctionPrologue`)
   - Дедупликация: разные кластеры могут резолвиться в одну функцию
   - Результат: вектор уникальных адресов RC4 функций (до `kMaxRC4Hooks=4`)
3. **Multi-hook installation**:
   - 4 отдельных naked stub'а (`HookedRC4Naked_0` — `HookedRC4Naked_3`), каждый с собственным trampoline
   - Каждый stub вызывает общий `RC4DetourHandler`, передавая ESP
   - MH_CreateHook/MH_EnableHook для каждой найденной функции
4. **Auto-detection calling convention** (6 вариантов, от специфичных к общим):
   - Convention 1: ECX=ctx, stk1=data, stk2=len (__thiscall)
   - Convention 3: ECX=ctx, stk1=len, stk2=data (variant)
   - Convention 4: EDX=ctx, stk1=data, stk2=len
   - Convention 5: EAX=ctx, stk1=data, stk2=len
   - Convention 6: EAX=ctx, stk1=len, stk2=data
   - Convention 2: stk1=ctx, stk2=data, stk3=len (__cdecl, последний — наименее специфичный)
   - Порядок проверки: регистровые конвенции первыми, stack-based последними (избежание false positive)
5. **Diagnostics**: первые 8 вызовов логируют ВСЕ регистры (EAX, ECX, EDX, EBX, ESI, EDI) + stk1-3
6. **ConsumePlaintext**: one-shot retrieval (thread-safe через CRITICAL_SECTION, InterlockedIncrement для callCount)
7. **Lifecycle**:
   - Installed: после MODULE_INITIALIZE (FindModuleInMemory) — до 4 хуков одновременно
   - Removed: на MODULE_USE и при Shutdown — все хуки снимаются
8. **Fallback**: если ни одна RC4 функция не найдена → используем S-box cloning (warden_rc4.cpp)

**Поддержка модулей с разными RC4 функциями**:
- Модуль **0BE6B21C**: RC4 использует EAX как context register (не ECX). Module thread может использовать отдельную RC4 функцию
- Multi-hook решает эту проблему: хукаем ВСЕ RC4 функции в модуле, convention detection определяет формат каждой

**Результаты**:
```
[rc4_hook] CMSG_WARDEN_DATA plaintext (21 bytes):
  01 00 15 00 [4-byte checksum] [HASH_RESULT data]
[rc4_hook] CMSG_WARDEN_DATA plaintext (8 bytes):
  02 00 01 00 [4-byte checksum] [CHEAT_CHECKS_RESULT]
```

**Статус**: ПОЛНОСТЬЮ РАБОТАЕТ (6/6 CMSG packets decrypted with [rc4_hook] tag)

---

## Что НЕ работает (TODO ✗)

### 1. Подмена ответов (spoofing)

**Описание**: модификация CMSG_WARDEN_DATA для обмана сервера.

**Цель**: отправлять "правильные" ответы, даже если у нас стоят хуки/читы.

**Примеры**:
- **MEM_CHECK**: вернуть хеш оригинальных байт (из Shadow Copy) вместо хеша с хуком
- **LUA_EVAL**: вернуть безопасный результат вместо реального (например, скрыть запрещённые аддоны)
- **PAGE_CHECK**: вернуть хеш оригинальной страницы памяти

**Текущая реализация**: отсутствует (мы только наблюдаем, не вмешиваемся)

**Текущий прогресс**:
- RC4 decryption: РАБОТАЕТ (через warden_rc4_hook)
- Парсинг CMSG: РАБОТАЕТ (request-response correlation через FIFO queue)
- Checksum algorithm: РЕШЁН (SHA1 → 5 × uint32_t LE → XOR-fold → uint32_t)
- Request-response correlation: РАБОТАЕТ (FIFO queue `std::deque<vector<PendingCheck>>`)

**Следующий шаг**: реализовать подмену данных в CMSG (пока только наблюдаем)

**Приоритет**: ВЫСОКИЙ (важно для полного bypass)

**Статус**: В РАЗРАБОТКЕ (наблюдение работает полностью, подмена не начата)

---

### 2. Remap cross-reference noise для некоторых модулей

**Описание**: remap cross-reference может производить 30+ noise типов для remap-only модулей.

**Пример**: модуль 46DCC0B8 — remap cross-ref выдал 33 типа, из них только 7 настоящих.

**Причина**: remap table содержит много служебных записей, не все записи — реальные type IDs.

**Решение**: `ExtractFromSingleRemap` + in-memory scan
- In-memory scan (primary) + dispatch chain дополнение из remap table
- `ExtractFromSingleRemap`: для одиночных remap tables — default handler (самый частый) отсеивается, остальные = type IDs
- Детерминированный парсер использует только типы, извлечённые из модуля
- Живой тест: 10/10 типов, 0 unknown types, все checksums VALID

**Митигация**: noise типы отсеиваются, парсинг строго по типам из модуля

**Приоритет**: НИЗКИЙ (in-memory scan решает проблему)

**Статус**: RESOLVED (не блокирует функциональность)

---

### 3. Парсинг CHEAT_CHECKS_RESULT

**Описание**: structured parsing per-check results в CMSG ответах.

**Формат CHEAT_CHECKS_RESULT** (confirmed):
```
[02] [resultLen:2 bytes LE] [checksum:4 bytes] [results:N bytes]
```

**Checksum algorithm**: SHA1(results) → 5 × uint32_t LE → XOR-fold → uint32_t
- Реализовано в `warden_checksum.cpp` через WinCrypt CALG_SHA1
- Все наблюдаемые checksums валидируются корректно

**Request-response correlation**: FIFO queue (`std::deque<vector<PendingCheck>>`)
- Warden отправляет несколько SMSG request'ов до получения CMSG response'ов
- Каждый request пушится в очередь (даже empty/truncated — для синхронизации)
- При получении CMSG — pop из очереди для корреляции
- Queue очищается на MODULE_USE

**Типы результатов** (confirmed по категориям):
- **TIMING**: 5 bytes always (flag:1 + ticks:4)
- **MEM_CHECK**: 1 byte if fail / 1+readLen bytes if OK
- **LUA_EVAL**: 1 byte if fail / 1+1+strlen bytes if OK
- **PAGE/PROC/MODULE/DRIVER**: 1 byte (0xE9=pass)
- **MPQ_CHECK**: 1 byte if fail / 1+20 bytes if OK

**Текущий статус**: корреляция и checksum validation РАБОТАЮТ, per-check result parsing — РАБОТАЕТ

**Приоритет**: СРЕДНИЙ (наблюдение готово, нужно перейти к spoofing)

**Статус**: РАБОТАЕТ (correlation + checksum + per-check parsing)

---

## К чему стремимся (ROADMAP)

### Выполнено (DONE ✓)

#### ~~1. Разбор формата CHEAT_CHECKS_RESULT~~
**Реализовано**: request-response correlation через FIFO queue, per-check result parsing по категориям, checksum validation.

#### ~~2. Алгоритм checksum~~
**Реализовано**: SHA1(results) → 5 × uint32_t LE → XOR-fold → uint32_t. Реализовано в `warden_checksum.cpp` через WinCrypt CALG_SHA1.

#### ~~3. Парсинг CMSG ответов~~
**Реализовано**: structured parsing с correlation. Результаты коррелируются с SMSG request'ами, delta и checksum проверяются.

---

### Ближайшее (1-2 недели)

#### 1. Подмена MEM_CHECK через Shadow Copy
**Цель**: Warden не видит наши хуки в .text секции

**Механизм**:
1. Перехватываем CHEAT_CHECKS_REQUEST (уже работает)
2. Парсим MEM_CHECK: адрес + длина (уже работает)
3. Проверяем: адрес в нашем hook list? (ScanForHookAddresses уже работает)
4. Если да:
   - Читаем оригинальные байты из Shadow Copy (уже работает)
   - Вычисляем SHA1 от оригинальных байт
   - Подменяем результат в CMSG: оригинальный хеш вместо хеша с хуком
   - Генерируем валидный checksum (алгоритм уже известен)
   - Шифруем RC4 (ключ из RC4 hook уже доступен)

**Критерий успеха**: Warden не банит, хотя у нас стоят хуки на проверяемых адресах

---

### Среднесрочное (1-2 месяца)

#### 2. Подмена LUA_EVAL результатов
**Цель**: скрыть запрещённые аддоны / модификации UI

**Пример LUA_EVAL**:
```lua
return GetAddOnInfo("SomeCheat")
```

Если аддон установлен → клиент вернёт его имя → бан.

**Механизм**:
1. Перехватываем LUA_EVAL в CHEAT_CHECKS_REQUEST (у нас уже есть hook на FrameScript_Execute)
2. Выполняем скрипт в sandboxed окружении (или вообще не выполняем)
3. Возвращаем безопасный результат (например, "nil")
4. Генерируем валидный checksum
5. Шифруем и отправляем spoofed CMSG

**Критерий успеха**: можем использовать запрещённые аддоны, Warden не видит

---

#### 3. Автоматическое определение опасности
**Цель**: знать заранее, какие адреса Warden проверяет → наши хуки в опасности?

**Механизм**:
1. Собираем статистику проверок (из всех CHEAT_CHECKS_REQUEST)
2. Если адрес нашего хука появляется в MEM_CHECK → ALERT
3. Считаем частоту: сколько раз в час Warden проверяет этот адрес
4. Выводим dashboard в консоль:
   ```
   FrameScript_Execute (0x00819210): checked 5 times in last hour [HIGH RISK]
   SendPacket (0x00632B50): checked 1 time [MEDIUM RISK]
   ARC4::Process (0x00774EA0): never checked [LOW RISK]
   ```

**Применение**: решать, стоит ли ставить хук на конкретный адрес (risk assessment)

**Критерий успеха**: dashboard обновляется в реальном времени

---

### Дальнее (3+ месяца)

#### 4. Полный Warden bypass
**Цель**: сервер думает, что у нас чистый клиент (нет хуков, читов, модификаций)

**Компоненты**:
- MEM_CHECK spoofing: ✓ (через Shadow Copy)
- LUA_EVAL spoofing: ✓ (безопасные результаты)
- PAGE_CHECK spoofing: ✗ (ещё не реализовано)
- HASH_REQUEST spoofing: ✗ (нужен анализ алгоритма)
- MODULE_CHECK spoofing: ✗ (проверка загруженных DLL — скрыть нашу DLL)
- DRIVER_CHECK spoofing: ✗ (проверка драйверов — если используем kernel-mode cheat)
- MPQ_CHECK spoofing: ✗ (проверка целостности game files)
- PROC_CHECK spoofing: ✗ (проверка списка процессов — скрыть подозрительные)

**Критерий успеха**: можем использовать любые читы, Warden никогда не банит

---

#### 5. PAGE_CHECK Evasion
**Цель**: обойти проверку защиты памяти (PAGE_EXECUTE_READ → PAGE_EXECUTE_READWRITE)

**Проблема**: если мы меняем protection на executable странице → Warden видит через VirtualQuery.

**Решение**:
- Hook на NtProtectVirtualMemory (kernel32!VirtualProtect → ntdll!NtProtectVirtualMemory)
- Временно меняем protection для записи, сразу возвращаем обратно
- Или используем hardware breakpoints (DR0-DR3) вместо inline hooks (не модифицируют память)

**Критерий успеха**: можем писать в .text секцию, PAGE_CHECK не видит изменений

---

#### 6. Антиопределение DLL
**Цель**: скрыть нашу DLL от Warden MODULE_CHECK

**Проблема**: Warden может вызывать NtQueryVirtualMemory → видит все загруженные модули.

**Решение**:
- Manual mapping: загружаем DLL без LoadLibrary (не попадает в PEB.Ldr lists)
- Hook на NtQueryVirtualMemory: скрываем наши регионы из результатов
- Или используем kernel-mode driver (DKOM — Direct Kernel Object Manipulation)

**Критерий успеха**: Warden не видит нашу DLL в списке модулей

---

## Метрики успеха

### Текущий прогресс
- **Функциональность**: 13/14 наблюдательных компонентов работают (93%)
- **Критические блокеры**: 0
- **Полнота анализа**: 15 модулей проанализировано (11/15 FULL dispatch chains, 1/15 partial, 3/15 remap-only)
- **Захват модулей**: 15+ модулей (0BE6B21C, 952860B1, 32F1D632, 46DCC0B8, CB9E43D6 и др.)
- **Извлечение типов**: in-memory scan как primary → 0 unknown types в живых тестах
- **RC4 CMSG**: расшифровка через internal hook — 100% success rate
- **Checksum**: SHA1 XOR-fold — РЕШЁН, все наблюдаемые checksums VALID
- **Request-response correlation**: FIFO queue — РАБОТАЕТ

### Следующий milestone
- MEM_CHECK spoofing: **ПРОТОТИП** (все компоненты ready: Shadow Copy, checksum, RC4 hook)

После этого: переход к полному bypass (LUA_EVAL spoofing, PAGE_CHECK evasion, DLL hiding).

---

## Заключение

Проект находится на стадии **готовности к активному spoofing**. Наблюдательная фаза практически завершена:

**Что мы умеем**:
- Перехватывать и парсить все типы Warden пакетов (SMSG и CMSG)
- Извлекать бинарные модули и их внутреннюю структуру (15+ модулей)
- Автоматически определять типы проверок из модуля (in-memory scan + dispatch chain + remap table)
- Расшифровывать CMSG ответы через internal RC4 hook (multi-hook, до 4 функций)
- Коррелировать SMSG requests с CMSG responses (FIFO queue)
- Валидировать checksums (SHA1 XOR-fold)
- Находить адреса наших хуков в запросах Warden (ScanForHookAddresses)
- Читать оригинальные байты .text секции (Shadow Copy)

**Критические прорывы**:
- **In-memory module scanning**: сканирование загруженного (распакованного, релоцированного) модуля — 0 unknown types
- **RLE unpacking**: для fallback-сканирования packed бинарников — убраны артефакты RLE control words
- **RC4 CMSG расшифровка** через multi-hook внутреннего PRGA (до 4 RC4 функций одновременно)
- **Checksum algorithm решён**: SHA1(results) → XOR-fold → uint32_t — все наблюдаемые checksums VALID
- **g_scanBaseAddr**: корректировка абсолютных адресов в in-memory модулях для remap table access

**Живые тесты** (2026-02-15):
- In-memory scan: 10/10 типов, 0 unknown, все checksums VALID
- Dispatch chain union + remap supplement: полное покрытие типов
- Request-response correlation: delta=0, все результаты разобраны по категориям

Следующая фаза: **активный spoofing**. Все компоненты для MEM_CHECK spoofing готовы (Shadow Copy, checksum, RC4 hook). Осталось реализовать подмену данных в CMSG.

Конечная цель: полная невидимость для Warden (чистый клиент с точки зрения сервера, но с произвольными модификациями на стороне клиента).
