# Deep Research: MEM_CHECK Spoofing для Warden в WoW 3.3.5a

## Задача

Реализовать **подмену ответов на MEM_CHECK проверки** Warden в клиенте WoW 3.3.5a (build 12340). Warden — это античит-система Blizzard, работающая через бинарные модули, загружаемые в процесс клиента. Мы перехватываем Warden-пакеты инжектированной DLL (x86). Задача: вместо реальных байт памяти (содержащих наши inline-hook патчи) подставить оригинальные байт из файла на диске, чтобы сервер не обнаружил модификации .text секции.

**Контекст**: это defensive security research, исследование протокола Warden на приватном сервере WoW 3.3.5a.

---

## Что нужно исследовать

### 1. Точка перехвата CMSG_WARDEN_DATA

**Главный вопрос**: на каком уровне и каким образом перехватывать и модифицировать исходящий CMSG_WARDEN_DATA перед отправкой серверу?

Есть несколько вариантов:

#### Вариант A: Модификация plaintext ДО шифрования (через RC4 hook)
- У нас есть хук на RC4 PRGA функцию ВНУТРИ модуля Warden (warden_rc4_hook.cpp).
- Этот хук вызывается в момент encrypt, когда plaintext buffer ещё доступен.
- **Вопрос**: можно ли в хуке RC4 подменить plaintext buffer in-place ПЕРЕД тем, как оригинальная PRGA функция его зашифрует? Тогда шифрование будет работать на уже подменённых данных, и RC4 stream cipher останется синхронизированным.
- **Проблема**: хук вызывается через naked detour. В момент вызова RC4DetourHandler мы видим plaintext через `dataPtr`. Но после return из detour оригинальная функция зашифрует то, что по этому адресу лежит. Если мы перезапишем данные по dataPtr — оригинальная функция зашифрует наши подменённые данные.

#### Вариант B: Модификация зашифрованного пакета в SendPacket hook
- У нас есть хук на SendPacket @ 0x00632B50.
- На этом уровне payload уже зашифрован RC4.
- **Вопрос**: можно ли расшифровать payload (у нас есть клонированный RC4 state), подменить нужные байты, пересчитать checksum, зашифровать обратно с тем же RC4 keystream?
- **Проблема**: для этого нужен RC4 encrypt state ТОЧНО в том же состоянии, в котором модуль зашифровал пакет. Любой десинхрон — и сервер получит мусор.

#### Вариант C: Подмена результатов в Warden module memory
- Warden модуль собирает результаты проверок в буфер, затем шифрует RC4.
- **Вопрос**: можно ли найти и подменить результаты MEM_CHECK в буфере модуля МЕЖДУ сборкой ответа и RC4-шифрованием?
- **Проблема**: нужен обратный инжиниринг каждого конкретного модуля для нахождения точки подмены.

#### Вариант D: Подмена памяти ДО чтения Warden модулем
- Warden модуль при MEM_CHECK читает N байт по указанному адресу.
- **Вопрос**: можно ли в момент обработки CHEAT_CHECKS_REQUEST (когда мы знаем какие адреса будут проверены) временно восстановить оригинальные байты на hook-адресах, дождаться пока модуль прочитает память, затем вернуть хуки обратно?
- **Проблема**: race condition — Warden модуль может читать асинхронно. Нужно точно знать момент чтения.

**Нужно исследовать**: какой из вариантов наиболее надёжный, какие подводные камни у каждого, есть ли известные реализации в open-source проектах (например, AzerothCore Warden, TrinityCore, HermesProxy, WowReeb, и др.).

### 2. Формат CMSG_WARDEN_DATA для CHEAT_CHECKS_RESULT

**Формат подтверждён** из нашего reverse engineering:

```
[0x02] [resultLen: 2 bytes LE] [checksum: 4 bytes] [results: resultLen bytes]
```

- `0x02` = WARDEN_CMSG_CHEAT_CHECKS_RESULT
- `resultLen` = длина секции результатов (uint16_t LE)
- `checksum` = SHA1(results) → 5 × uint32_t LE → XOR-fold → uint32_t
- `results` = конкатенация результатов для каждой проверки из SMSG request, в том же порядке

**Формат результата MEM_CHECK**:
- Если OK: `[0x00] [memory_data: readLen bytes]` — первый байт 0x00 = success, затем readLen байт памяти
- Если fail: `[errorCode]` — один байт != 0x00

**Нужно исследовать**:
- AzerothCore / TrinityCore серверный код, который проверяет CHEAT_CHECKS_RESULT — что именно сервер делает с данными MEM_CHECK? Он считает SHA1 от полученных байт и сравнивает с ожидаемым? Или проверяет конкретные байты?
- Какой формат expected result на сервере для MEM_CHECK? Есть ли seed/salt?
- Как именно сервер детектирует inline hook? По SHA1 hash от readLen байт? По конкретным байт-паттернам?

### 3. RC4 stream cipher синхронизация

**Критический вопрос**: RC4 — это stream cipher. Каждый байт, зашифрованный/расшифрованный, продвигает внутреннее состояние (i, j, S-permutation). Если мы модифицируем plaintext и зашифруем его "другим" RC4 потоком — состояние десинхронизируется, и ВСЕ последующие пакеты будут мусором.

**Нужно исследовать**:
- Один ли RC4 state используется для всех CMSG_WARDEN_DATA, или каждый пакет шифруется независимо?
- Если один state — то при подмене plaintext в варианте A (до encrypt) синхронизация сохраняется, потому что мы меняем plaintext, а не ciphertext.
- При варианте B (модификация ciphertext в SendPacket) — нужно XOR старый plaintext, XOR новый plaintext на тех же позициях keystream. Это эквивалентно XOR (old_plain ^ new_plain) на ciphertext. RC4 state продвинулся одинаково в обоих случаях — ОК?
- Или нужно полностью перешифровать пакет с нуля?

### 4. Определение какие MEM_CHECK адреса совпадают с нашими хуками

**Уже реализовано**: `ScanForHookAddresses()` в hooks.cpp сканирует check section на 4-byte LE паттерны наших hook-адресов. Также в `ParseCheatChecksRequest()` для каждого MEM_CHECK парсится addr + readLen, и проверяется пересечение с нашими hook-адресами.

**Наши hook-адреса**:
- `FrameScript_Execute` @ 0x00819210 (MinHook inline hook, ~5-14 bytes JMP patch)
- `SMSG_WARDEN_DATA handler` @ 0x007DA850 (MinHook inline hook)
- `SendPacket` @ 0x00632B50 (MinHook inline hook)
- `ARC4::Process` @ 0x00774EA0 (MinHook inline hook)

**Нужно исследовать**:
- Сколько именно байт MinHook перезаписывает при inline hook? Типично 5 (JMP rel32), но может быть больше если оригинальная инструкция длиннее 5 байт.
- Как определить readLen для конкретного MEM_CHECK — это поле уже парсится в ParseCheatChecksRequest.
- Нужно ли подменять ВСЕ байты результата, или только те, которые отличаются от оригинала?

### 5. Чтение оригинальных байт из Shadow Copy

**Уже реализовано**: `shadow::GetCleanBytes(runtimeAddr, out, len)` — читает оригинальные байты .text секции из файла Wow.exe, маппированного в память через CreateFileMapping + MapViewOfFile.

**Нужно исследовать**:
- Достаточно ли просто прочитать clean bytes и отправить их вместо реальных? Или сервер ожидает SHA1 hash?
- Формат MEM_CHECK result: `[0x00][raw_bytes]` или `[0x00][SHA1(raw_bytes)]`?

### 6. Checksum

**Уже реализован**: `warden_checksum::BuildChecksum(data, length)` — SHA1(data) → 5 × uint32_t LE → XOR-fold → uint32_t.

**Нужно исследовать**:
- Checksum считается от ВСЕЙ секции results (не от каждого check-а отдельно). Значит, при подмене одного MEM_CHECK результата нужно пересчитать checksum для всей новой results секции.
- Подтвердить, что формат: `[0x02][new_resultLen:2][new_checksum:4][new_results:N]`

### 7. Серверная сторона (AzerothCore / TrinityCore)

**Нужно исследовать исходный код серверов**:
- Как сервер генерирует MEM_CHECK запросы? Какие адреса проверяет, с какой частотой?
- Как сервер обрабатывает MEM_CHECK результат? Сравнивает raw bytes? SHA1? Или и то и другое?
- Что именно вызывает бан/дисконнект при failed MEM_CHECK?
- Есть ли таймаут на ответ CHEAT_CHECKS_RESULT?
- Файлы для анализа:
  - `src/server/game/Warden/WardenWin.cpp` (TrinityCore / AzerothCore)
  - `src/server/game/Warden/WardenCheckMgr.cpp`
  - `src/server/game/Warden/Warden.cpp`
  - `src/server/game/Warden/WardenWin.h`

### 8. Известные реализации (open-source)

**Нужно найти и проанализировать**:
- Существующие Warden bypass реализации (GitHub, форумы)
- Как другие проекты решают проблему MEM_CHECK spoofing?
- Используется ли где-то подход "подмена перед RC4" vs "подмена после RC4"?
- Ключевые слова для поиска: "warden bypass", "warden spoof", "mem check bypass", "wow anti-cheat bypass", "CDataStore modify", "warden rc4 decrypt"

---

## Полный рабочий контекст

### Архитектура проекта

```
wotlk-utils/
├── injector/          — Console app (x86) для inject/eject DLL
├── shared/            — Общий код (logging, props)
└── wotlk/             — DLL (x86), инжектируемая в WoW 3.3.5a
    ├── dllmain.cpp    — Точка входа DLL
    ├── hooks/
    │   ├── hooks.h    — API: Initialize() / Shutdown()
    │   └── hooks.cpp  — 4 хука + полный Warden парсинг (1427 строк)
    └── warden/
        ├── warden_types.h       — Enum-ы: opcodes, check types, data sizes
        ├── warden_scan.h/.cpp   — Извлечение type IDs из модулей (2084 строк)
        ├── warden_rc4_hook.h/.cpp — RC4 PRGA hook внутри модуля (820 строк)
        ├── warden_rc4.h/.cpp    — S-box scanning fallback (722 строки)
        ├── warden_checksum.h/.cpp — SHA1 XOR-fold checksum (54 строки)
        ├── shadow_copy.h/.cpp   — PE mapping Wow.exe для clean bytes (152 строки)
        └── module_dump.h/.cpp   — Захват и кэширование модулей на диск
```

### Целевой процесс

- **WoW 3.3.5a build 12340** — 32-bit Windows PE (x86)
- **ImageBase**: 0x00400000
- **.text section**: RVA=0x1000, VSize=0x5DD3B3
- Warden модули загружаются через VirtualAlloc (MEM_PRIVATE, PAGE_EXECUTE_READWRITE)

### Хуки (MinHook, inline patching)

**4 активных хука**:

| Hook | Address | Convention | Purpose |
|------|---------|------------|---------|
| FrameScript_Execute | 0x00819210 | __cdecl | Lua execution logging |
| SMSG_WARDEN_DATA | 0x007DA850 | naked (non-standard) | Warden packet interception |
| SendPacket | 0x00632B50 | naked | CMSG capture + RC4 decrypt |
| ARC4::Process | 0x00774EA0 | __thiscall | Session cipher diagnostics |

MinHook использует JMP rel32 (5 bytes) для inline hook. Если оригинальная инструкция <5 bytes, MinHook может патчить больше (до ~14 bytes) для atomic trampoline.

**Временное отключение хуков в WardenPreHandler**:
Перед выполнением оригинального обработчика Warden, мы вызываем `MH_DisableHook` для FrameScript_Execute и SMSG_WARDEN_DATA. Это **восстанавливает оригинальные байты** на этих адресах, чтобы Warden не видел JMP-патчи при чтении памяти. После обработчика — `MH_EnableHook` возвращает хуки.

**ВАЖНО**: SendPacket и ARC4::Process НЕ отключаются в WardenPreHandler (они нужны для CMSG capture). Если Warden проверяет их адреса — они будут видны.

### Протокол Warden (SMSG → CMSG)

#### SMSG_WARDEN_DATA (сервер → клиент, opcode 0x2E8)

Первый байт = тип команды:
- `0x00` MODULE_USE — запрос загрузки модуля (SHA1 + RC4 key)
- `0x01` MODULE_CACHE — чанк данных модуля
- `0x02` CHEAT_CHECKS_REQUEST — массив проверок
- `0x03` MODULE_INITIALIZE — финализация загрузки модуля
- `0x05` HASH_REQUEST — запрос SHA1 хеша модуля

#### CMSG_WARDEN_DATA (клиент → сервер, opcode 0x2E7)

Первый байт = тип ответа:
- `0x00` MODULE_MISSING — модуль не найден в кэше
- `0x01` MODULE_OK — модуль загружен
- `0x02` CHEAT_CHECKS_RESULT — результаты проверок
- `0x04` MEM_CHECKS_RESULT
- `0x05` HASH_RESULT — SHA1 хеш модуля

**Payload зашифрован RC4** (Warden module internal cipher, НЕ session cipher).

#### CHEAT_CHECKS_REQUEST (0x02) формат

```
[0x02] [string_section] [check_section] [xorByte]
```

- **String section**: `[len:1][string:len]` повторяется для LUA/MPQ/DRIVER/PROC строк
- **Terminator**: `0x00` байт после последней строки
- **Check section**: `[type^xorByte:1][data:N]` повторяется для каждой проверки
- **xorByte**: последний байт пакета, XOR-ится только с type bytes (НЕ с data)

#### CHEAT_CHECKS_RESULT (0x02) формат

```
[0x02] [resultLen:2 LE] [checksum:4] [results:resultLen bytes]
```

**Checksum**: `SHA1(results) → reinterpret as 5 × uint32_t LE → XOR-fold → uint32_t`

**Results section** — конкатенация per-check результатов в порядке запроса:

| Category | OK format | Fail format |
|----------|-----------|-------------|
| TIMING | `[flag:1][ticks:4]` — всегда 5 bytes | — |
| MEM | `[0x00][memory:readLen]` | `[errorCode:1]` |
| PAGE | `[0xE9]` (1 byte) | `[0xE9]` (1 byte) |
| PROC | `[0xE9]` (1 byte) | `[0xE9]` (1 byte) |
| MODULE | `[0xE9]` (1 byte) | `[0xE9]` (1 byte) |
| DRIVER | `[0xE9]` (1 byte) | `[0xE9]` (1 byte) |
| MPQ | `[0x00][SHA1:20]` | `[errorCode:1]` |
| LUA | `[0x00][strlen:1][string:N]` | `[errorCode:1]` |

**Для MEM_CHECK**: результат содержит **RAW BYTES** памяти (не SHA1!). readLen определяется из SMSG request.

### RC4 расшифровка CMSG

**Два механизма** (работают параллельно):

#### 1. Internal RC4 hook (PRIMARY) — warden_rc4_hook.cpp

Хукает RC4 PRGA функцию **внутри** бинарного модуля Warden. Захватывает plaintext buffer ПЕРЕД шифрованием.

**Как работает**:
1. После MODULE_INITIALIZE сканируем runtime-память модуля на инструкции с displacement 0x100/0x101 (i/j в [S[256]][i][j] layout)
2. Группируем в кластеры, находим function prologue (push ebp; mov ebp, esp)
3. Устанавливаем MinHook на каждую найденную RC4 функцию (до 4 одновременно)
4. Naked detour → RC4DetourHandler: auto-detect calling convention (6 вариантов), читаем plaintext buffer, валидируем как CMSG, сохраняем в g_capturedPlaintext
5. ConsumePlaintext() — one-shot retrieval в SendPacketHandler

**Calling convention auto-detection** (порядок проверки):
1. ECX=ctx, stk1=data, stk2=len (__thiscall)
2. ECX=ctx, stk1=len, stk2=data (variant)
3. EDX=ctx, stk1=data, stk2=len
4. EAX=ctx, stk1=data, stk2=len
5. EAX=ctx, stk1=len, stk2=data
6. stk1=ctx, stk2=data, stk3=len (__cdecl, last — least specific)

**CMSG structural validation** перед сохранением:
- MODULE_MISSING/OK: len == 1
- CHEAT_CHECKS_RESULT: len == 7 + resultLen (resultLen from bytes 1-2)
- HASH_RESULT: len == 21

#### 2. S-box cloning (FALLBACK) — warden_rc4.cpp

Сканирует всю MEM_PRIVATE память процесса на RC4 S-box permutations (256 уникальных byte values).

**Как работает**:
1. В WardenPreHandler (до обработки Warden) вызываем CloneAllStates() — читаем i/j/S для каждого кандидата
2. В SendPacketHandler пробуем каждый клон на расшифровку CMSG
3. Structural validation определяет, какой клон — encrypt state
4. Найденный encrypt state переходит в "running" режим (не нужно re-clone каждый раз)

### Shadow Copy — чтение оригинальных байт

`shadow_copy.cpp` маппит Wow.exe с диска и парсит PE headers.

**API**:
```cpp
bool shadow::Initialize();              // Map Wow.exe, parse PE, find .text
void shadow::Shutdown();                // Unmap
bool shadow::GetCleanBytes(addr, out, len);  // Read original bytes at runtime addr
bool shadow::IsInTextSection(addr);     // Check if addr is in .text
```

**Формула конвертации**: `fileOffset = textRawOff + (runtimeAddr - imageBase - textRVA)`

**Текущий статус**: инициализируется при загрузке DLL, РАБОТАЕТ. Пока используется только для диагностики, не для spoofing.

### Request-Response correlation

FIFO queue `std::deque<std::vector<PendingCheck>>`:
- Каждый SMSG CHEAT_CHECKS_REQUEST пушит вектор PendingCheck в очередь
- Каждый CMSG CHEAT_CHECKS_RESULT pop-ит первый элемент
- Warden может отправить НЕСКОЛЬКО requests перед получением responses — queue это решает

**PendingCheck struct**:
```cpp
struct PendingCheck {
    uint8_t       realType;  // module-specific type ID
    CheckCategory category;  // TIMING, MEM, PAGE, PROC, MODULE, DRIVER, MPQ, LUA
    uint8_t       readLen;   // MEM_CHECK: memory read length
    std::string   context;   // e.g. "0x00819210 len=20"
};
```

### Жизненный цикл одной Warden сессии

1. Сервер отправляет **MODULE_USE** (SHA1 hash + RC4 key модуля)
2. Если модуль не в кэше → серия **MODULE_CACHE** пакетов (чанки)
3. **MODULE_INITIALIZE** → мы:
   - Находим модуль в памяти (сигнатура 26 bytes)
   - In-memory scan → извлекаем check type IDs
   - Устанавливаем RC4 hook внутри модуля
   - Запускаем S-box scan (fallback)
4. Серия **CHEAT_CHECKS_REQUEST** → мы парсим, собираем PendingCheck-и
5. Клиент отвечает **CHEAT_CHECKS_RESULT** (зашифрован RC4) → мы расшифровываем через hook или S-box clone, парсим per-check results
6. Повторяем 4-5 каждые ~30 секунд
7. При смене модуля → возврат к шагу 1

### Warden модули (15+ захвачено)

Модули — **НЕ PE файлы**. Формат:
- 40-byte header: moduleSize, relocOff, relocCount, exportTableOff, exportCount, baseIndex, importTableOff, importLibCount, sectionDescCount
- RLE-packed sections
- Delta-encoded relocations
- Загружаются через VirtualAlloc (MEM_PRIVATE, PAGE_EXECUTE_READWRITE)

Check type IDs **module-specific** — каждый модуль использует свои значения для TIMING, MEM, PAGE, etc.

Мы извлекаем type IDs через XOR-anchored dispatch chain scan:
- Anchor: `32 [40-7F, rm≠4] 04` (xor r8, [reg+4])
- Затем `movzx eax, al`
- Затем BFS обход дерева сравнений (cmp + je/jne/jg/jl)
- Дополнение из remap tables

**Фиксированные data sizes** (одинаковы для всех модулей):
- TIMING=0, MPQ=1, LUA=1, MEM=6, MODULE=24, DRIVER=25, PAGE=29, PROC=31

### Текущее поведение WardenPreHandler

В `hooks.cpp` → `WardenPreHandler()`:

```cpp
static void __cdecl WardenPreHandler(uintptr_t savedEsp)
{
    g_insideWardenHandler = true;

    // Clone RC4 S-boxes BEFORE handler encrypts CMSG
    if (warden_rc4::HasEncryptState() || warden_rc4::HasCandidates())
        warden_rc4::CloneAllStates();

    // Disable hooks so Warden sees clean memory at hook addresses
    if (!g_hooksDisabled) {
        g_hooksDisabled = true;
        MH_DisableHook(kFrameScriptExecute);
        MH_DisableHook(kWardenHandler);
    }

    // Save CDataStore context + hijack return address
    uint32_t* s = (uint32_t*)savedEsp;
    g_savedCDataStore = (void*)s[13];
    ...
    g_savedRetAddr = (void*)s[9];
    s[9] = (uint32_t)g_wardenPostHandlerAddr;
}
```

**Ключевое наблюдение**: MH_DisableHook **восстанавливает оригинальные байты** на адресах хуков. Это значит что в момент, когда Warden handler выполняется (между Pre и Post), оригинальные байты на месте для FrameScript_Execute и SMSG_WARDEN_DATA. Но SendPacket и ARC4::Process остаются пропатченными.

---

## Ключевые вопросы для ресерча

1. **Формат MEM_CHECK result на серверной стороне**: сервер ожидает raw bytes или SHA1? Исследовать AzerothCore/TrinityCore `WardenWin.cpp`.

2. **Оптимальная точка перехвата CMSG**: plaintext modification (вариант A — через RC4 hook buffer), ciphertext modification (вариант B — через SendPacket с XOR-patch), или temporary unhook (вариант D)?

3. **RC4 stream cipher синхронизация**: при модификации plaintext в варианте A — сохранится ли синхронизация? При XOR-patch в варианте B — правильна ли арифметика `new_cipher = old_cipher ^ old_plain ^ new_plain`?

4. **MinHook patch size**: точно сколько байт MinHook перезаписывает для каждого из наших 4 хуков? Нужно ли знать конкретную длину для правильного сравнения с readLen из MEM_CHECK?

5. **Race conditions**: в варианте D (temporary unhook) — может ли Warden модуль читать память из другого потока? Если да — unhook/re-hook на main thread не поможет.

6. **Серверные таймауты**: есть ли таймаут на CHEAT_CHECKS_RESULT? Если мы задержим ответ для подмены — не вызовет ли это дисконнект?

7. **Checksum пересчёт**: при подмене MEM_CHECK результата нужно пересчитать checksum для ВСЕЙ results секции. Это значит нужно знать plaintext ВСЕХ results (не только MEM_CHECK). Как получить plaintext всех results?

8. **Множественные MEM_CHECK в одном запросе**: один CHEAT_CHECKS_REQUEST может содержать несколько MEM_CHECK. Нужно подменить результат для КАЖДОГО, который попадает на наш hook.

9. **Порядок операций**: SMSG request → Warden module reads memory + builds response → RC4 encrypt → SendPacket. В какой именно момент мы вмешиваемся? Нужна точная timeline.

10. **Существующие реализации**: есть ли open-source Warden bypass для 3.3.5a, реализующий MEM_CHECK spoofing? Если да — какой подход используется?

---

## Ожидаемый результат ресерча

1. **Рекомендация по подходу** (вариант A/B/C/D или комбинация) с обоснованием
2. **Детальный алгоритм подмены** с учётом RC4 синхронизации
3. **Анализ серверного кода** (AzerothCore/TrinityCore): что именно проверяет сервер при MEM_CHECK
4. **Перечень рисков** и edge cases (race conditions, timing, desync)
5. **Примеры кода** или ссылки на существующие реализации
6. **Рекомендация по архитектуре**: где в нашем коде разместить логику spoofing (новый файл? модификация hooks.cpp? модификация warden_rc4_hook.cpp?)

---

## Дополнительный контекст

### Полный исходный код ключевых файлов

Файлы доступны для анализа:
- `wotlk/hooks/hooks.cpp` (1427 строк) — ВСЕ хуки + парсинг SMSG/CMSG + request-response correlation
- `wotlk/warden/warden_types.h` (118 строк) — opcodes и enum-ы
- `wotlk/warden/shadow_copy.cpp` (152 строки) — PE mapping для clean bytes
- `wotlk/warden/warden_rc4_hook.cpp` (820 строк) — RC4 internal hook
- `wotlk/warden/warden_rc4.cpp` (722 строки) — S-box scanner fallback
- `wotlk/warden/warden_checksum.cpp` (54 строки) — SHA1 XOR-fold checksum
- `wotlk/warden/warden_scan.cpp` (2084 строки) — type extraction из модулей
- `wotlk/warden/module_dump.cpp` — module capture + disk cache
- `wotlk/dllmain.cpp` (100 строк) — DLL entry point + lifecycle

### Зависимости
- MinHook (x86-windows-static-md via vcpkg)
- glog (x86-windows via vcpkg)
- miniz (third_party, zlib decompress)
- Windows CryptoAPI (SHA1 для checksum)

### Сборка
- Visual Studio, Win32 (x86)
- Конфигурации: Debug|Win32, Release|Win32, Debug Eject|Win32
