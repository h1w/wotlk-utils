# Deep Research: DRIVER_CHECK / MPQ_CHECK / PROC_CHECK Spoofing для Warden в WoW 3.3.5a

## Задача

Реализовать **подмену ответов на DRIVER_CHECK, MPQ_CHECK и PROC_CHECK проверки** Warden в клиенте WoW 3.3.5a (build 12340). Мы инжектированная DLL (x86), перехватывающая и модифицирующая Warden-пакеты. Все три типа проверок уже **полностью парсятся** (SMSG request) и **корректно читаются** (CMSG response через RC4 hook), но результаты сейчас проходят без модификации (pass-through). Задача: добавить селективный spoofing для этих трёх типов проверок.

**Контекст**: defensive security research, исследование протокола Warden на приватном сервере WoW 3.3.5a (Warmane).

---

## Что уже работает (полный контекст)

### Инфраструктура spoofing

Вся инфраструктура для подмены CMSG уже реализована и работает в живых тестах:

1. **RC4 hook внутри Warden модуля** (`warden_rc4_hook.cpp`): перехватывает RC4 PRGA функцию внутри бинарного модуля, даёт доступ к plaintext ДО шифрования. 100% success rate.

2. **SpoofCmsgIfNeeded()** (`warden_spoof.cpp`): вызывается из RC4 hook handler ПЕРЕД шифрованием. Copy-based rebuild: читает старые результаты, пишет новые, модифицируя при необходимости. Пересчитывает checksum.

3. **Request-response корреляция**: FIFO queue (`std::deque<vector<PendingCheck>>`) — каждый SMSG CHEAT_CHECKS_REQUEST пушит вектор PendingCheck в очередь, CMSG pop-ает первый элемент. Работает даже при множественных request'ах до ответа.

4. **Checksum**: SHA1(results) → 5 × uint32_t LE → XOR-fold → uint32_t. Реализовано в `warden_checksum.cpp`. Все checksums валидируются.

5. **Shadow Copy** (`shadow_copy.cpp`): PE mapping Wow.exe с диска для чтения оригинальных байт .text секции.

6. **PEB Unlinking** (`peb_unlink.cpp`): скрытие DLL из PEB.Ldr lists для MODULE_CHECK evasion.

### Уже реализованный spoofing (рабочий)

| Тип проверки | Подход | Статус |
|-------------|--------|--------|
| MEM_CHECK | Shadow copy: заменяем байты памяти на оригинальные из Wow.exe на диске | РАБОТАЕТ |
| PAGE_CHECK | Force 0xE9 (pass) для проверок на адресах наших хуков | РАБОТАЕТ |
| MODULE_CHECK | PEB unlinking (primary) + force 0xE9 (backup) | РАБОТАЕТ |
| LUA_EVAL | Селективный: подмена addon-detection результатов на empty string | РАБОТАЕТ |
| HASH_REQUEST/RESULT | Deferred RC4 hook install — модуль вычисляет hash на чистом коде | РАБОТАЕТ |

### Текущая обработка DRIVER / MPQ / PROC в коде

В `warden_spoof.cpp` → `SpoofCmsgIfNeeded()`:

```cpp
case CheckCategory::PROC:
case CheckCategory::DRIVER:
    // Fixed 1-byte result — pass-through (no modification)
    newResults[newPos++] = oldResults[oldPos++];
    break;

case CheckCategory::MPQ:
    if (resultByte != 0x00) {
        // Fail — 1 byte pass-through
        newResults[newPos++] = oldResults[oldPos++];
    } else {
        // Success: [0x00][SHA1:20] — pass-through
        if (oldPos + 21 > resultLen) goto done;
        std::memcpy(newResults + newPos, oldResults + oldPos, 21);
        oldPos += 21; newPos += 21;
    }
    break;
```

В `hooks.cpp` → `ParseCheatChecksRequest()`, парсинг SMSG request:

```cpp
// DRIVER: seed(4)+SHA1(20)+stringIndex(1) = 25 bytes
} else if (dataSize == 25) {
    uint8_t strIdx = data[pos + 24];
    pending.category = CheckCategory::DRIVER;
    if (strIdx < strings.size())
        pending.context = strings[strIdx]; // driver name, e.g. "vmmemctl"
}

// PROC: seed(4)+SHA1(20)+modIdx(1)+procIdx(1)+addr(4)+readLen(1) = 31 bytes
} else if (dataSize == 31) {
    uint8_t modIdx  = data[pos + 24];
    uint8_t procIdx = data[pos + 25];
    uint32_t addr;
    memcpy(&addr, data + pos + 26, 4);
    uint8_t readLen = data[pos + 30];
    pending.category = CheckCategory::PROC;
    // context = "0xADDR ModuleName!ProcName"
}

// MPQ/LUA: stringIndex(1) = 1 byte
} else if (dataSize == 1) {
    uint8_t strIdx = data[pos];
    // Classified by content: MPQ if contains '\' and file extension, otherwise LUA
    pending.category = CheckCategory::MPQ; // or LUA
    pending.context = strings[strIdx]; // e.g. "World\\Maps\\Azeroth\\Azeroth_32_32.adt"
}
```

### PendingCheck struct

```cpp
struct PendingCheck {
    uint8_t       realType;   // module-specific type ID
    CheckCategory category;   // TIMING, MEM, PAGE, PROC, MODULE, DRIVER, MPQ, LUA
    uint8_t       readLen;    // MEM/PAGE: byte count to read
    uint32_t      checkAddr;  // MEM/PAGE: target address (0 for others)
    std::string   context;    // human-readable: driver name, MPQ path, proc context
};
```

### Формат результатов в CMSG (подтверждено reverse engineering)

```
CMSG CHEAT_CHECKS_RESULT:
  [0x02] [resultLen:2 LE] [checksum:4] [results:N]

Per-check результаты (в порядке SMSG request):
  TIMING:  [flag:1][ticks:4] — всегда 5 bytes
  MEM:     fail=[errorCode:1], OK=[0x00][data:readLen]
  PAGE:    [0xE9] (1 byte) — pass/fail
  PROC:    [0xE9] (1 byte) — pass/fail
  MODULE:  [0xE9] (1 byte) — pass/fail
  DRIVER:  [0xE9] (1 byte) — pass/fail
  MPQ:     fail=[errorCode:1], OK=[0x00][SHA1:20]
  LUA:     fail=[errorCode:1], OK=[0x00][strlen:1][string:N]
```

### Формат SMSG запросов (check data)

```
Check data sizes (фиксированные, одинаковы для всех модулей):
  TIMING  = 0 bytes  (нет данных)
  MPQ     = 1 byte   (stringIndex)
  LUA     = 1 byte   (stringIndex)
  MEM     = 6 bytes  (unk:1 + addr:4 + readLen:1)
  MODULE  = 24 bytes (seed:4 + SHA1:20)
  DRIVER  = 25 bytes (seed:4 + SHA1:20 + stringIndex:1)
  PAGE    = 29 bytes (seed:4 + SHA1:20 + addr:4 + readLen:1)
  PROC    = 31 bytes (seed:4 + SHA1:20 + modIdx:1 + procIdx:1 + addr:4 + readLen:1)
```

### Наблюдаемые данные из живых тестов

#### DRIVER_CHECK примеры:
```
string[0] (DRIVER) "vmmemctl"      ← VMware memory balloon driver
string[1] (DRIVER) "VBoxMiniRdrDN" ← VirtualBox redirector
string[2] (DRIVER) "vmci"          ← VMware communication interface
string[3] (DRIVER) "vboxguest"     ← VirtualBox guest additions
string[4] (DRIVER) "vmhgfs"        ← VMware Host-Guest File System
string[5] (DRIVER) "prl_fs"        ← Parallels filesystem
string[6] (DRIVER) "VBoxSF"        ← VirtualBox shared folders
string[7] (DRIVER) "VBoxGuest"     ← VirtualBox guest additions (alt name)

check#N DRIVER strIdx=0 name="vmmemctl"   → result=0xE9 (pass = not found)
check#N DRIVER strIdx=1 name="VBoxMiniRdrDN" → result=0xE9 (pass)
```

В наших тестах ВСЕ DRIVER_CHECK возвращают 0xE9 (не найден), потому что мы запускаем на реальном железе (не в VM). Но если бы запустили в VirtualBox — vboxguest вернул бы != 0xE9.

#### MPQ_CHECK примеры:
```
string[0] (MPQ) "World\\Maps\\Azeroth\\Azeroth_32_32.adt"
string[1] (MPQ) "World\\Maps\\Kalimdor\\Kalimdor_28_42.adt"
...

check#N MPQ strIdx=0 str="World\\Maps\\Azeroth\\Azeroth_32_32.adt"
  → result=0x00 (OK) SHA1=[AB CD EF ... 20 bytes]
```

MPQ_CHECK: клиент открывает файл из MPQ архива, считает SHA1 от содержимого, отправляет. Сервер сравнивает с эталонным SHA1. Проверяет целостность игровых файлов (модифицированные карты, текстуры = чит).

#### PROC_CHECK примеры:
```
string[0] (STRING) "kernel32.dll"
string[1] (STRING) "OpenProcess"
string[2] (STRING) "ntdll.dll"
string[3] (STRING) "NtReadVirtualMemory"

check#N PROC modIdx=0 procIdx=1 addr=0x7C801D7B len=8 mod="kernel32.dll" proc="OpenProcess"
  → result=0xE9 (pass)
```

PROC_CHECK: проверяет наличие конкретного exported function в конкретной DLL по адресу + сравнивает байты пролога. result=0xE9 означает "функция не найдена" или "пролог совпадает с ожидаемым". Если кто-то хукнул kernel32!OpenProcess — PROC_CHECK это обнаружит.

---

## Что нужно исследовать

### 1. DRIVER_CHECK — серверная логика и клиентская реализация

**Главные вопросы**:

a) **Как ИМЕННО клиент (Warden модуль) выполняет DRIVER_CHECK?**
   - Использует ли `NtQuerySystemInformation(SystemModuleInformation)` для перечисления драйверов ядра?
   - Или `CreateFileW("\\\\.\\driverName")` для проверки доступности device?
   - Или `EnumDeviceDrivers` / другой WinAPI?
   - Что возвращает: существует/не существует? Или данные об обнаруженном драйвере?

b) **Формат ответа DRIVER_CHECK в деталях:**
   - Мы наблюдаем 1 byte: 0xE9 = "pass" (driver not found). Какое значение = "fail" (driver found)?
   - При "fail" — что отправляется? Только 1 byte или дополнительные данные?
   - Исследовать AzerothCore / TrinityCore серверный код: `WardenWin.cpp` → `HandleData()` или аналог.

c) **Серверная валидация:**
   - Что делает сервер при DRIVER_CHECK fail? Immediate disconnect? Delayed ban? Log only?
   - На Warmane — как обрабатываются DRIVER_CHECK fails?

d) **Стратегия spoofing:**
   - Если мы в VM (VMware/VirtualBox/Parallels) — нужно скрыть VM драйверы. Force 0xE9?
   - Безопасно ли ВСЕГДА возвращать 0xE9 для DRIVER_CHECK? Или есть "reverse checks" где 0xE9 = подозрительно?
   - Какие конкретно драйверы проверяются? Только VM? Или ещё cheat-tool drivers?
   - seed(4)+SHA1(20) в запросе DRIVER — для чего? Сервер отправляет ожидаемый SHA1?

e) **Нужно найти**: исходный код клиентской стороны Warden, выполняющий DRIVER_CHECK. Есть ли в open source (TrinityCore WardenWin.cpp server-side, WowReeb, HermesProxy, skullsecurity wiki)?

### 2. MPQ_CHECK — серверная логика и клиентская реализация

**Главные вопросы**:

a) **Как ИМЕННО клиент выполняет MPQ_CHECK?**
   - Открывает файл через SFileOpenFileEx из Storm.dll (внутренняя MPQ библиотека)?
   - Читает содержимое файла из MPQ архива?
   - Считает SHA1 от полного содержимого файла?
   - Или SHA1 от первых N байт? Или от конкретного блока?
   - Используется ли seed из запроса для SHA1-HMAC?

b) **Формат ответа MPQ_CHECK:**
   - OK: `[0x00][SHA1:20]` — SHA1 от содержимого файла
   - Fail: `[errorCode:1]` — файл не найден или ошибка чтения
   - Это ТОЛЬКО SHA1 всего файла? Или может быть другой формат?

c) **Серверная валидация:**
   - Сервер хранит эталонный SHA1 для каждого MPQ файла?
   - Что происходит при mismatch? Модифицированные карты = бан?
   - Исследовать `WardenCheckMgr.cpp` — как формируется check list, какие MPQ файлы проверяются.

d) **Стратегия spoofing:**
   - Нужно ли вообще спуфить MPQ_CHECK? Если мы не модифицируем MPQ файлы — результат будет правильным.
   - Если игрок использует кастомные карты/текстуры — как спуфить?
   - Вариант 1: Пропустить (pass-through) — если не модифицируем MPQ, результат естественно правильный.
   - Вариант 2: Кешировать SHA1 оригинальных файлов, отвечать кешированными значениями.
   - Вариант 3: Читать оригинальные файлы из backup-копии MPQ.
   - Seed в запросе — влияет ли он на SHA1 вычисление? Если да — нельзя кешировать.

e) **Критический вопрос: Используется ли seed из запроса?**
   - DRIVER и PAGE запросы содержат seed(4)+SHA1(20). Для PAGE: seed используется сервером для HMAC.
   - Для MPQ: seed(4) отсутствует в запросе (data size = 1 byte = только stringIndex). Подтвердить.
   - Значит ли это, что MPQ_CHECK — просто прямой SHA1 от файла?

f) **Нужно найти**: точную реализацию MPQ_CHECK в Warden модуле, серверный код валидации в TC/AC.

### 3. PROC_CHECK — серверная логика и клиентская реализация

**Главные вопросы**:

a) **Как ИМЕННО клиент выполняет PROC_CHECK?**
   - `GetModuleHandleA(moduleName)` → `GetProcAddress(handle, procName)` → чтение readLen байт по адресу?
   - Или проверяет, что функция существует + сравнивает пролог с SHA1?
   - Что означает addr в PROC запросе? Это ожидаемый адрес функции? Или offset?
   - seed(4)+SHA1(20) — сервер отправляет ожидаемый SHA1 пролога?

b) **Формат ответа PROC_CHECK в деталях:**
   - 0xE9 = pass (функция не найдена, или пролог совпадает с ожидаемым)?
   - Что такое "pass" для PROC_CHECK? Что именно проверяется?
   - Когда result != 0xE9? Что означают другие значения?
   - При fail — отправляются ли байты пролога (как MEM_CHECK) или только result byte?

c) **Серверная валидация:**
   - Сервер проверяет наличие/отсутствие конкретной функции?
   - Или сравнивает пролог функции с эталоном?
   - Что происходит если kernel32!OpenProcess хукнут (cheat tool)?
   - Что происходит если DLL не загружена (функция не найдена)?

d) **Стратегия spoofing:**
   - Мы НЕ хукаем kernel32/ntdll функции (наши хуки только в Wow.exe). Нужен ли spoofing?
   - Если мы не хукаем системные DLL — PROC_CHECK пройдёт честно. Но для полноты...
   - Если кто-то хукнул OpenProcess из другого софта (антивирус, overlay, Discord) — PROC_CHECK может дать false positive? Нужно ли тогда спуфить?
   - Нужно ли хранить "чистые" байты прологов системных функций?
   - Можно ли читать пролог из файла на диске (как shadow copy для Wow.exe, но для kernel32.dll)?

e) **Поле addr в PROC запросе:**
   - В наших данных: `addr=0x7C801D7B` для kernel32!OpenProcess
   - Это абсолютный адрес? Совпадает ли с реальным GetProcAddress?
   - Что если DLL загружена по другому адресу (ASLR)? 3.3.5a клиент 32-bit, ASLR?
   - Сервер откуда берёт этот адрес? Захардкожен? Или вычисляется?

f) **Нужно найти**: серверный код формирования PROC_CHECK запроса, клиентскую реализацию, формат "expected result".

### 4. Общие вопросы для всех трёх типов

a) **Seed + SHA1 в DRIVER и PROC запросах:**
   - Оба содержат `seed(4) + SHA1(20)` в check data
   - SHA1 = ожидаемый хеш чего? Самого файла? Пролога функции? Имени драйвера?
   - Seed = для HMAC? Для salted hash? Или seed используется для чего-то другого?
   - Как сервер формирует эти seed+SHA1 пары? Исследовать `WardenCheckMgr.cpp`.

b) **Существует ли формат "extended result" для DRIVER/PROC?**
   - Мы наблюдаем только 1-byte результаты (0xE9). Есть ли вариант с дополнительными данными?
   - В AC/TC серверном коде — как разбирается ответ? Всегда 1 byte?

c) **Приоритетность и порядок проверок:**
   - Сервер может отправлять смешанные проверки: 5 MEM + 2 DRIVER + 1 MPQ + 1 PROC в одном пакете
   - Наш парсер корректно обрабатывает все типы в произвольном порядке

d) **Warmane-специфичные особенности:**
   - Warmane может использовать собственные warden checks (не стандартные TC/AC)?
   - Есть ли информация о Warmane-специфичных DRIVER/MPQ/PROC проверках?
   - Custom Warden модули Warmane vs стандартные TC/AC?

---

## Серверный код для анализа

### TrinityCore (3.3.5a branch)

Ключевые файлы серверной реализации Warden:
- `src/server/game/Warden/WardenWin.cpp` — формирование запросов + обработка ответов
- `src/server/game/Warden/WardenWin.h` — структуры и определения
- `src/server/game/Warden/WardenCheckMgr.cpp` — менеджер проверок, загрузка из БД
- `src/server/game/Warden/WardenCheckMgr.h` — типы проверок
- `src/server/game/Warden/Warden.cpp` — базовый класс
- `src/server/game/Warden/Warden.h` — базовые определения

Нужно найти:
1. **Как формируется DRIVER_CHECK запрос**: откуда берётся seed, SHA1, driver name
2. **Как формируется MPQ_CHECK запрос**: откуда SHA1, file path
3. **Как формируется PROC_CHECK запрос**: откуда seed, SHA1, module name, proc name, addr, readLen
4. **Как валидируются ответы** на все три типа: memcmp? SHA1 comparison? existence check?
5. **Penalty policy**: disconnect? ban? log? configurable?

### AzerothCore (3.3.5a)

Аналогичные файлы:
- `src/server/game/Warden/WardenWin.cpp`
- `src/server/game/Warden/WardenCheckMgr.cpp`

AzerothCore может иметь дополнительные проверки или модификации.

### Warmane (если информация доступна)

- Warmane fork на базе TrinityCore. Серверный код закрыт.
- Информация может быть доступна через community wiki, форумы, Discord.

---

## Полный рабочий контекст проекта

### Архитектура проекта

```
wotlk-utils/
├── injector/          — Console app (x86) для inject/eject DLL
├── shared/            — Общий код (logging via glog, props)
└── wotlk/             — DLL (x86), инжектируемая в WoW 3.3.5a
    ├── dllmain.cpp    — Точка входа DLL
    ├── hooks/
    │   ├── hooks.h    — API: Initialize() / Shutdown()
    │   └── hooks.cpp  — 4 хука + полный Warden парсинг + request-response correlation
    └── warden/
        ├── warden_types.h       — Enum-ы: opcodes, check types, data sizes
        ├── warden_scan.h/.cpp   — Извлечение type IDs из модулей
        ├── warden_spoof.h/.cpp  — CMSG spoofing (MEM/PAGE/MODULE/LUA + будущие DRIVER/MPQ/PROC)
        ├── warden_rc4_hook.h/.cpp — RC4 PRGA hook внутри модуля (multi-hook)
        ├── warden_rc4.h/.cpp    — S-box scanning fallback
        ├── warden_checksum.h/.cpp — SHA1 XOR-fold checksum
        ├── shadow_copy.h/.cpp   — PE mapping Wow.exe для чистых байт
        ├── module_dump.h/.cpp   — Module capture + disk cache
        └── peb_unlink.h/.cpp    — PEB.Ldr unlinking для MODULE_CHECK evasion
```

### Целевой процесс

- **WoW 3.3.5a build 12340** — 32-bit Windows PE (x86)
- **ImageBase**: 0x00400000
- **.text section**: RVA=0x1000, VSize=0x5DD3B3
- ASLR: отключен (старый 32-bit компилятор)
- Warden модули загружаются через VirtualAlloc (MEM_PRIVATE, PAGE_EXECUTE_READWRITE)
- Формат модулей: НЕ PE, кастомный (40-byte header, RLE sections, delta relocs)

### Наши хуки (MinHook inline patching)

| Hook | Address | Convention | Purpose |
|------|---------|------------|---------|
| FrameScript_Execute | 0x00819210 | __cdecl | Lua execution logging |
| SMSG_WARDEN_DATA | 0x007DA850 | naked | Warden packet interception (return-address hijack) |
| SendPacket | 0x00632B50 | naked | CMSG capture + RC4 decrypt + logging |
| ARC4::Process | 0x00774EA0 | __thiscall | Session cipher diagnostics |

**ВАЖНО**: Мы хукаем ТОЛЬКО функции Wow.exe. Мы НЕ хукаем kernel32.dll, ntdll.dll или любые другие системные DLL. Это означает что PROC_CHECK на системных DLL не должен обнаружить наши хуки.

### Протокол: как spoofing работает сейчас

1. Сервер отправляет SMSG CHEAT_CHECKS_REQUEST
2. `ParseCheatChecksRequest()` парсит, создаёт `vector<PendingCheck>`, пушит в FIFO queue
3. Warden модуль выполняет проверки, формирует CMSG CHEAT_CHECKS_RESULT
4. Модуль вызывает свою RC4 PRGA функцию для шифрования CMSG
5. **Наш RC4 hook** перехватывает вызов, видит plaintext buffer ПЕРЕД шифрованием
6. `SpoofCmsgIfNeeded()` вызывается из RC4 hook handler:
   - Peek front of FIFO queue (PendingChecks для текущего ответа)
   - Walk results: для каждого check → copy/modify по категории
   - MEM: если адрес совпадает с hook target → подменяем на shadow copy bytes
   - PAGE: если адрес совпадает → force 0xE9
   - MODULE: если result != 0xE9 → force 0xE9
   - LUA: если addon-detection → empty string
   - **DRIVER / PROC: pass-through (пока)**
   - **MPQ: pass-through (пока)**
   - Если modified → update resultLen, copy back, zero trailing, recompute checksum
7. RC4 hook returns, оригинальная PRGA шифрует MODIFIED plaintext
8. SendPacket отправляет зашифрованный пакет серверу

### Жизненный цикл Warden сессии

1. MODULE_USE → Reset всех state (RC4 hooks, scan, pending checks, hash seed)
2. MODULE_CACHE × N → сборка модуля из чанков
3. MODULE_INITIALIZE → scan type IDs, find module in memory
4. HASH_REQUEST → deferred RC4 hook install (ПОСЛЕ hash computation!)
5. CHEAT_CHECKS_REQUEST × N → парсинг, push PendingChecks
6. CMSG CHEAT_CHECKS_RESULT × N → spoofing через RC4 hook
7. При смене модуля → возврат к шагу 1

---

## Конкретные сценарии для spoofing

### Сценарий 1: DRIVER_CHECK в VM

Игрок запускает WoW в VirtualBox. Warden отправляет DRIVER_CHECK с name="vboxguest".
- Реальный результат: != 0xE9 (драйвер найден)
- Нужный результат: 0xE9 (драйвер не найден)
- Spoofing: force 0xE9 для всех DRIVER_CHECK? Или только для VM-related?

### Сценарий 2: MPQ_CHECK с модифицированными файлами

Игрок модифицировал текстуры (кастомные adt файлы). Warden отправляет MPQ_CHECK.
- Реальный результат: SHA1 модифицированного файла
- Нужный результат: SHA1 оригинального файла
- Spoofing: нужно иметь доступ к оригинальным файлам. Backup MPQ? Вычисление SHA1 из оригинала?

### Сценарий 3: PROC_CHECK с хукнутой системной функцией

Другой софт (антивирус, overlay) хукнул kernel32!OpenProcess. Warden отправляет PROC_CHECK.
- Реальный результат: != 0xE9 (пролог модифицирован)
- Нужный результат: 0xE9 (чистый пролог)
- Spoofing: читать чистые байты из kernel32.dll на диске? Или force 0xE9?

### Сценарий 4: Наш PROC_CHECK

Warden проверяет функцию в Wow.exe (не системная DLL). Наш хук на этом адресе.
- Вопрос: PROC_CHECK проверяет ТОЛЬКО системные DLL или может проверять Wow.exe?
- Если Wow.exe — нужна shadow copy подмена. Если только системные — не наша проблема.

---

## Ожидаемый результат ресерча

1. **Детальный алгоритм клиентской стороны** для каждого из 3 типов проверок:
   - Какие WinAPI вызывает Warden модуль для DRIVER_CHECK, MPQ_CHECK, PROC_CHECK
   - Точный формат формирования result bytes

2. **Детальный алгоритм серверной стороны** (из TC/AC source code):
   - Как формируется запрос (seed, SHA1, parameters)
   - Как валидируется ответ
   - Penalty policy при fail

3. **Рекомендации по стратегии spoofing** для каждого типа:
   - DRIVER: force 0xE9? selective? blacklist/whitelist?
   - MPQ: pass-through? cache original SHA1? backup MPQ?
   - PROC: shadow copy для системных DLL? force 0xE9? когда нужен spoofing?

4. **Анализ рисков и edge cases**:
   - Может ли "всегда 0xE9" для DRIVER/PROC быть подозрительным?
   - Может ли сервер отправлять "reverse checks" (ожидающие != 0xE9)?
   - Race conditions, timing concerns

5. **Конкретные изменения в коде** (архитектура):
   - Что добавить в `PendingCheck` struct? Дополнительные поля?
   - Какие новые функции/файлы нужны?
   - Нужна ли shadow copy для DLL (kernel32.dll и др.)?
   - Нужен ли доступ к MPQ файлам?

6. **Warmane-специфичная информация** (если доступна):
   - Какие конкретно DRIVER/MPQ/PROC проверки используются
   - Отличия от стандартного TC/AC

---

## Дополнительный контекст

### Полный исходный код ключевых файлов (доступен для анализа)

- `wotlk/hooks/hooks.cpp` (1474 строки) — хуки + парсинг + request-response correlation
- `wotlk/warden/warden_spoof.cpp` (467 строк) — core spoofing (MEM/PAGE/MODULE/LUA + DRIVER/MPQ/PROC pass-through)
- `wotlk/warden/warden_spoof.h` (65 строк) — public API + PendingCheck struct
- `wotlk/warden/warden_types.h` (120 строк) — opcodes, check types, data sizes
- `wotlk/warden/warden_rc4_hook.cpp` (820+ строк) — RC4 PRGA hook, SpoofCmsgIfNeeded вызов
- `wotlk/warden/warden_checksum.cpp` (54 строки) — SHA1 XOR-fold
- `wotlk/warden/shadow_copy.cpp` (152 строки) — PE mapping для clean bytes
- `wotlk/warden/peb_unlink.cpp` — PEB.Ldr unlinking

### Зависимости

- MinHook (x86-windows-static-md via vcpkg) — inline hooking
- glog (x86-windows via vcpkg) — logging
- miniz (third_party) — zlib decompression
- Windows CryptoAPI — SHA1 для checksum

### Сборка

- Visual Studio, Win32 (x86)
- Debug|Win32, Release|Win32, Debug Eject|Win32
