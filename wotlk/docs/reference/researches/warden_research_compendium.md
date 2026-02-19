# Компендиум исследований: Система Warden в World of Warcraft 3.3.5a (Build 12340)

**Полная техническая спецификация античит-системы, протоколов, криптографии и механизмов обхода**

---

## 1. Введение

### 1.1. Роль Warden в экосистеме 3.3.5a

Система Warden представляет собой динамически подгружаемый античит-модуль, разработанный компанией Blizzard Entertainment для обеспечения целостности игрового процесса World of Warcraft. В версии 3.3.5a (Build 12340, эра дополнения Wrath of the Lich King) используется вторая или ранняя третья итерация протокола Warden, характеризующаяся полиморфизмом модулей и challenge-response архитектурой.

В отличие от современных античит-решений уровня ядра (Ring 0), Warden 3.3.5a функционирует исключительно в пользовательском пространстве (Ring 3), инжектируясь в адресное пространство процесса Wow.exe. Система не является статическим компонентом исполняемого файла — модуль передается сервером по сети во время сессии, что позволяет обновлять логику детекции без патчей клиента.

**Ключевые характеристики:**
- Размер модуля после декомпрессии: ~28–30 KB
- Частота проверок: каждые 10–60 секунд
- Криптография: RC4 (потоковый шифр), SHA-1 (хеширование), RSA-2048 (подпись модуля)
- Типов проверок: 9 стандартных категорий
- Полиморфизм: check type ID изменяются между модулями

### 1.2. Философия полиморфизма

**Критическое отличие от статических античит-систем:** каждый модуль Warden использует собственные значения байтов для идентификации типов проверок. Тип `TIMING_CHECK` может быть представлен байтом `0x57` в одном модуле и `0x74` в другом. Это принципиальное проектное решение, делающее невозможным универсальный статический анализ.

Эмуляторы серверов (TrinityCore, AzerothCore, MaNGOS) обходят эту проблему, захардкодив значения из одного конкретного reverse-engineered модуля (`79C0768D657977D697E10BAD956CCED1`), но это работает только если сервер распространяет именно этот канонический модуль.

### 1.3. Цели и методология исследования

Данный компендиум представляет собой синтез 13 независимых исследований, охватывающих:

1. **Бинарный формат модуля** — структура заголовков, RLE-упаковка, релокации, импорт
2. **Криптографический транспорт** — SRP6, RC4, SHA-1, RSA-2048
3. **Протокол обмена данными** — структура пакетов SMSG/CMSG, опкоды, сериализация
4. **Система типов проверок** — 9 категорий, размеры данных, форматы ответов
5. **Механизмы диспетчеризации** — jump tables, binary search trees, remap tables
6. **Извлечение Type ID из модулей** — статический анализ, символьное исполнение, SMT-солверы
7. **Реализация на стороне сервера** — база данных `warden_checks`, планировщик, валидация
8. **Методы обхода (spoofing)** — Shadow Copy, RC4 hooking, checksum recalculation

Методология объединяет:
- Обратную разработку (reverse engineering) бинарных модулей через IDA Pro/Ghidra
- Анализ исходного кода open-source эмуляторов
- Перехват и анализ сетевого трафика
- Динамический анализ с использованием hooking frameworks (MinHook, Detours)

---

## 2. Архитектура системы Warden

### 2.1. Жизненный цикл сессии

Система Warden функционирует через state machine с семью состояниями:

```
STATE_INITIAL
    ↓ (login успешен)
STATE_REQUESTED_MODULE
    ↓ (SMSG_WARDEN_DATA: MODULE_INFO)
STATE_SENT_MODULE
    ↓ (модуль загружен и расшифрован)
STATE_REQUESTED_HASH
    ↓ (HASH_REQUEST отправлен)
STATE_INITIALIZE_MODULE
    ↓ (HASH_RESULT получен, ключи пересгенерированы)
STATE_REQUESTED_DATA
    ↓ (CHEAT_CHECKS_REQUEST отправлен)
STATE_RESTING
    ↓ (цикл: каждые 10-60 секунд → STATE_REQUESTED_DATA)
```

**Ключевые пакеты:**

| Направление | Опкод | Warden Inner Opcode | Назначение |
|-------------|-------|---------------------|------------|
| S→C | SMSG_WARDEN_DATA (0x2E6) | 0x00 | MODULE_INFO |
| S→C | SMSG_WARDEN_DATA | 0x01 | MODULE_CACHE (chunk) |
| S→C | SMSG_WARDEN_DATA | 0x02 | CHEAT_CHECKS_REQUEST |
| S→C | SMSG_WARDEN_DATA | 0x03 | MODULE_INITIALIZE |
| S→C | SMSG_WARDEN_DATA | 0x05 | HASH_REQUEST |
| C→S | CMSG_WARDEN_DATA (0x2E7) | 0x00 | MODULE_MISSING |
| C→S | CMSG_WARDEN_DATA | 0x01 | MODULE_OK |
| C→S | CMSG_WARDEN_DATA | 0x02 | CHEAT_CHECKS_RESULT |
| C→S | CMSG_WARDEN_DATA | 0x05 | HASH_RESULT |

### 2.2. Динамическая загрузка модуля

**Критическое отличие от PE:** модуль Warden не использует стандартный формат Portable Executable (PE). Это кастомный формат Blizzard без MZ/PE сигнатур.

**Процесс загрузки:**

1. **Transmission pipeline:**
   - RC4 decrypt (с использованием session key)
   - Verify RSA-2048 signature (`SHA1(data + "MAIEV.MOD")`)
   - zlib inflate (декомпрессия)

2. **Manual mapping:**
   - `VirtualAlloc` с флагом `PAGE_EXECUTE_READWRITE` (RWX)
   - Тип аллокации: `MEM_COMMIT | MEM_PRIVATE` (~30KB)
   - Не отображается в `CreateToolhelp32Snapshot` по умолчанию
   - Один непрерывный блок памяти для всего модуля

3. **Loader operations:**
   - RLE unpacking секций
   - Delta-encoded relocations processing
   - Import resolution (по именам и ординалам)
   - Entry point calculation и вызов init функции

---

## 3. Криптографический слой

### 3.1. SRP6 (Secure Remote Password)

Протокол SRP6 используется для установления сессии без передачи пароля в открытом виде.

**Математическая модель:**

```
Клиент вычисляет:
    u = H(A, B)
    x = H(s, H(I, ":", P))
    S = (B - k · g^x)^(a + u · x) mod N

Сервер вычисляет:
    S = (A · v^u)^b mod N

где:
    H = SHA-1
    N = большое простое число (1024-bit или 2048-bit)
    g = генератор группы (обычно 2 или 5)
    k = множитель (обычно 3)
    s = соль (salt)
    v = g^x mod N (верификатор, хранится на сервере)
    a, b = случайные эфемерные ключи
    A = g^a mod N (публичный ключ клиента)
    B = k·v + g^b mod N (публичный ключ сервера)
```

Общий секрет `S` используется как энтропия для генерации session key.

### 3.2. RC4 потоковое шифрование

**Критическая особенность:** Warden использует **персистентный RC4 state** — один экземпляр S-box на всю сессию для каждого направления (SMSG и CMSG).

#### Генерация ключей

**Метод SHA1 XOR-Fold:**

```cpp
// Псевдокод из AzerothCore
keyData = SHA1(ServerSeed[16] + ClientSeed[16] + ModuleKey)
uint32 checkSum = 0;
for (uint8 i = 0; i < 5; ++i)
    checkSum ^= keyData.ints[i];  // XOR пять uint32_t слов
// Финальный 16-байтный ключ = первые 16 байт keyData
RC4_Init(key[16]);
```

#### Архитектура S-box

**Два отдельных контекста:**
- `_inputCrypto` — для расшифровки S→C пакетов
- `_outputCrypto` — для шифрования C→S пакетов

**Структура контекста:**
```c
struct RC4Context {
    uint8_t S[256];  // Permutation S-box
    uint8_t i;       // Index at offset +0x100
    uint8_t j;       // Index at offset +0x101
};
```

**КРИТИЧЕСКИ ВАЖНО:** Keystream генерируется независимо от plaintext. Изменение plaintext перед encryption не нарушает синхронизацию, но изменение длины пакета приводит к десинхронизации S-box между клиентом и сервером.

### 3.3. RSA-2048 подпись модуля

**Формат контейнера после RC4 decrypt:**
```
[4 bytes] Uncompressed Length
[N bytes] Compressed Data (zlib)
[4 bytes] "NGIS" Marker (0x4E474953)
[256 bytes] RSA Signature
```

**Алгоритм проверки:**
1. Compute `SHA1(CompressedData + "MAIEV.MOD")`
2. Pad hash to 256 bytes: `0xBB 0xBB ... 0xBB [20-byte SHA1]`
3. Verify: `signature^e mod N == padded_hash`
   - `e = 65537` (стандартный exponent)
   - `N = 256-byte modulus` (одинаковый для WoW 1.12.1 через 3.3.5a)

Без приватного ключа Blizzard невозможно создать валидные кастомные модули.

---

## 4. Бинарный формат модуля Warden

### 4.1. 40-байтный заголовок (0x28)

| Offset | Size | Field | Описание |
|--------|------|-------|----------|
| 0x00 | 4 | `dwModuleSize` | Общий размер модуля в памяти после распаковки секций |
| 0x04 | 4 | Reserved | Зарезервировано/неиспользуется |
| 0x08 | 4 | `dwRelocDataOffset` | Смещение до данных релокаций |
| 0x0C | 4 | `dwRelocCount` | Количество релокаций |
| 0x10 | 4 | `dwExportTableOffset` | Смещение до таблицы экспорта |
| 0x14 | 4 | `dwExportCount` | Количество экспортируемых функций |
| 0x18 | 4 | `dwBaseIndex` | Базовый индекс для вычисления entry point |
| 0x1C | 4 | `dwImportTableOffset` | Смещение до таблицы импорта |
| 0x20 | 4 | `dwImportLibraryCount` | Количество импортируемых библиотек |
| 0x24 | 4 | `dwSectionDescCount` | Количество дескрипторов секций |

### 4.2. Секции и RLE-упаковка

**Дескрипторы секций** начинаются с offset `0x28`, каждый 12 байт.

**Packed data** начинается с offset `0x28 + (dwSectionDescCount × 12)`.

**RLE scheme (alternating copy/skip):**

```
Инициализация: mode = COPY
Цикл:
    Читаем uint16_t length (little-endian)
    Если mode == COPY:
        Копируем length байт из источника в destination
        mode = SKIP
    Иначе (mode == SKIP):
        Заполняем length байт нулями в destination
        mode = COPY
    Повторяем до заполнения dwModuleSize байт
```

Первый run всегда COPY. Чередование продолжается до конца.

### 4.3. Релокации (delta-encoded)

**Два формата:**

1. **2-byte relative delta** (high bit clear):
   ```
   offset_delta = (byte[0] << 8) | byte[1]
   current_offset += offset_delta
   ```

2. **4-byte absolute offset** (high bit set):
   ```
   offset = ((byte[0] & 0x7F) << 24) | (byte[1] << 16) |
            (byte[2] << 8) | byte[3]
   current_offset = offset
   ```

На каждом `current_offset`, прибавить `module_base_address` к DWORD:
```c
*(DWORD*)(module + current_offset) += module_base_address;
```

Идентично PE `.reloc` processing.

### 4.4. Импорт (по имени и ординалу)

**Формат 8-byte descriptor:**
```
[4 bytes] Offset to library name string (null-terminated)
[4 bytes] Offset to IAT (Import Address Table)
```

**IAT формат:**
```
Array of DWORD entries, terminated by 0x00000000
  Положительное значение: offset to function name string
  Отрицательное значение: (value & 0x7FFFFFFF) = ordinal number
```

**Resolution:**
```c
HMODULE hLib = LoadLibraryA(libraryName);
for each IAT entry:
    if (entry > 0):
        funcPtr = GetProcAddress(hLib, functionName);
    else:
        funcPtr = GetProcAddress(hLib, MAKEINTRESOURCEA(entry & 0x7FFFFFFF));
    IAT[i] = funcPtr;  // Перезаписываем в IAT
```

Типичные импорты: `kernel32.dll`, `user32.dll`.

### 4.5. Точка входа и таблицы функций

**Вычисление entry point:**

```c
DWORD EBP = header[0x18];           // dwBaseIndex
DWORD EDX = 1 - EBP;                // Adjusted export index
DWORD ECX = header[0x10];           // dwExportTableOffset
DWORD entryRVA = *(DWORD*)(module + ECX + EDX * 4);
fnInitializeModule init = (fnInitializeModule)(module + entryRVA);
```

**Calling convention:** `__fastcall` (ECX = first param, EDX = second param)

#### Host Callback Table (7 функций, 28 байт)

Передается модулю при инициализации:

| Offset | Function | Назначение |
|--------|----------|-----------|
| +0x00 | `SendPacket` | Отправка CMSG_WARDEN_DATA на сервер |
| +0x04 | `CheckModule` | Проверка модуля в памяти |
| +0x08 | `LoadModule` | Загрузка дополнительных модулей |
| +0x0C | `AllocateMemory` | VirtualAlloc wrapper |
| +0x10 | `ReleaseMemory` | VirtualFree wrapper |
| +0x14 | `SetRC4Data` | Установка RC4 ключей |
| +0x18 | `GetRC4Data` | Получение RC4 контекста |

#### Module Export Table (4 функции)

Возвращается как `WardenFuncList**`:

| Offset | Function | Convention | Назначение |
|--------|----------|------------|-----------|
| +0x00 | `GenerateRC4Keys` | `__thiscall` | Генерация RC4 ключей из seed |
| +0x04 | `UnloadModule` | `__thiscall` | Очистка при выгрузке |
| +0x08 | `PacketHandler` | `__thiscall` | Обработка CHEAT_CHECKS_REQUEST |
| +0x0C | `Tick` | `__thiscall` | Периодический вызов |

**Контекстная структура:**

```c
struct WardenContext {
    WardenFuncList** ppFuncList;   // +0x00
    uint8_t          xorByte;      // +0x04 (последний байт пакета)
    uint8_t          _padding[3];
    void*            moduleBase;   // +0x08
    RC4Context*      rc4Decrypt;   // +0x0C
    RC4Context*      rc4Encrypt;   // +0x10
    // ... дополнительные поля
};
```

### 4.6. Runtime память

**Allocation characteristics:**
- Один `VirtualAlloc` вызов
- Флаги: `MEM_COMMIT | PAGE_EXECUTE_READWRITE`
- Тип: `MEM_PRIVATE` (не `MEM_IMAGE`)
- Размер: `dwModuleSize` байт (~30KB)
- Весь модуль (header + code + data + imports + relocs) в одном блоке

**Stable signature (memcpy-like функция):**
```
56 57 FC 8B 54 24 14 8B 74 24 10 8B 44 24 0C 8B CA 8B F8 C1 E9 02 74 02 F3 A5
```

Эта сигнатура появляется во всех модулях и используется для copy memory при сканировании. Не полиморфна.

**Client-side handler:**
```
WardenClient_HandlePacket @ 0x006CA5C0  (WoW 3.3.5a)
```

---

## 5. Протокол обмена данными

### 5.1. SMSG_WARDEN_DATA Опкоды

| Опкод | Имя | Направление | Payload |
|-------|-----|-------------|---------|
| 0x00 | MODULE_INFO | S→C | `[hash:16][len:4][blob:N]` |
| 0x01 | MODULE_CACHE | S→C | `[MD5:16][chunk:N]` chunked transfer |
| 0x02 | CHEAT_CHECKS_REQUEST | S→C | String table + checks |
| 0x03 | MODULE_INITIALIZE | S→C | Function pointers |
| 0x05 | HASH_REQUEST | S→C | `[seed:16]` SHA1 challenge |

### 5.2. CMSG_WARDEN_DATA Опкоды

| Опкод | Имя | Направление | Payload |
|-------|-----|-------------|---------|
| 0x00 | MODULE_MISSING | C→S | `[0x00]` 1 byte |
| 0x01 | MODULE_OK | C→S | `[0x01]` 1 byte |
| 0x02 | CHEAT_CHECKS_RESULT | C→S | `[len:2][checksum:4][results:N]` |
| 0x05 | HASH_RESULT | C→S | `[SHA1:20]` hash of seed |

### 5.3. WARDEN_SMSG_MODULE_INITIALIZE (0x03)

Предоставляет модулю Warden адреса функций WoW.exe для вызова:

| Function | Offset (от 0x00400000) | Описание |
|----------|------------------------|----------|
| SFileOpenFile | 0x006485F0 | Storm.dll — открытие MPQ файлов |
| SFileGetFileSize | 0x006487F0 | Получение размера файла |
| SFileReadFile | 0x00648460 | Чтение из MPQ |
| SFileCloseFile | 0x00648730 | Закрытие файла |
| FrameScript::GetText | 0x00819D40 | Получение Lua переменной |
| PerformanceCounter | 0x0046AE20 | Таймер высокой точности |

**ВАЖНО:** Адреса специфичны для Build 12340. Другие билды имеют другие адреса.

### 5.4. HASH_REQUEST (0x05) и RC4 re-key

**КРИТИЧЕСКАЯ ОПАСНОСТЬ:** Это единственный пакет, изменение которого вызывает немедленный disconnect.

**Процесс:**

1. Сервер отправляет: `[opcode:1=0x05][seed:16]`
2. Модуль вычисляет: `SHA1_hash = SHA1(seed)`
3. Модуль отправляет: `[opcode:1=0x05][SHA1_hash:20]`
4. **Обе стороны** пересоздают RC4 states:
   ```cpp
   _inputCrypto.Init(SHA1_hash, 20);
   _outputCrypto.Init(SHA1_hash, 20);
   ```

**Почему нельзя модифицировать:**

Модуль уже сохранил свой вычисленный `SHA1_hash` для re-key. Если hooking code подменит `SHA1_hash` в CMSG на другое значение:
- Сервер сгенерирует новые RC4 keys с **подмененным** hash
- Модуль сгенерирует новые RC4 keys со **своим оригинальным** hash
- Результат: разные S-box states → все последующие пакеты garbled → disconnect

**Решение:** Логировать для диагностики, но **НИКОГДА не модифицировать**.

### 5.5. CHEAT_CHECKS_REQUEST (опкод 0x02) — полная структура

#### Секция 1: Таблица строк (String Table)

**Формат:**
```
Offset 0: [opcode:1 = 0x02]
Offset 1: Цикл:
    Read uint8 len
    If len == 0: конец таблицы, переход к секции проверок
    Read len bytes (string)
    stringTable[index++] = string
```

Строки индексируются с 0. Используются для:
- Имена драйверов (DRIVER_CHECK)
- Имена DLL (MODULE_CHECK, PROC_CHECK)
- Имена функций (PROC_CHECK)
- Имена MPQ файлов (MPQ_CHECK)
- Lua код (LUA_EVAL_CHECK — стандартный формат)

**Завершение:** Байт `0x00` после последней строки.

#### Секция 2: Проверки (не-MEM_CHECK)

**XOR encoding type bytes:**
```
wire_byte = canonical_type_byte ^ xorByte
где xorByte = _inputKey[0] = последний байт пакета
```

**Формат каждой проверки:**
```
[type_byte ^ xorByte : 1]
[type-specific data : Variable]
```

**ВАЖНО:** Только type byte XOR'ится. Data bytes (адреса, хеши) — plaintext.

**Последовательность:**
1. TIMING_CHECK всегда первый (0 data bytes)
2. Произвольное количество других проверок
3. **Терминатор:** raw `xorByte` (1 байт) — декодируется как `xorByte ^ xorByte = 0x00`

#### Секция 3: MEM_CHECK

**Без XOR encoding.** Каждая запись:
```
[moduleNameIndex : 1]  // Обычно 0x00 для WoW.exe
[address : 4 LE]
[length : 1]
```

**Всего 6 байт** на проверку.

Секция MEM_CHECK идет **после** секции других проверок (после xorByte terminator).

#### XOR-byte и завершение пакета

**Последний байт расшифрованного пакета** = `xorByte`.

**Client-side decoding:**
```cpp
uint8_t xorByte = packet[packet.size - 1];
for each check:
    uint8_t encType = ReadUInt8();
    uint8_t realType = encType ^ xorByte;
    DispatchToHandler(realType);
```

### 5.6. CHEAT_CHECKS_RESULT (CMSG, опкод 0x02)

**Полная структура:**

```
[opcode : 1 = 0x02]
[resultLen : 2 LE]      // Размер results section
[checksum : 4 LE]       // SHA1 XOR-fold
[results : resultLen]   // Результаты всех проверок
```

#### Checksum алгоритм

```cpp
uint32_t BuildChecksum(uint8_t* data, uint32_t length) {
    uint8_t hash[20];
    SHA1(data, length, hash);

    uint32_t checksum = 0;
    for (uint8_t i = 0; i < 5; ++i) {
        uint32_t word = *(uint32_t*)(hash + i * 4);  // LE
        checksum ^= word;
    }
    return checksum;
}
```

**Применяется к:** Весь `results` section (`resultLen` байт).

#### Ordering правила

**TIMING_CHECK всегда первый:**
```
[result:1][ticks:4]  = 5 bytes
```

**Остальные проверки в порядке request:**

Сервер отслеживает порядок в `_CurrentChecks` vector. Клиент **ДОЛЖЕН** возвращать результаты в том же порядке.

**Размеры результатов по типам** (см. раздел 6.2).

---

## 6. Система типов проверок

### 6.1. Сводная таблица размеров (Request)

**Фиксированные размеры данных после type byte:**

| Check Type | Hex ID | Data Bytes | Структура |
|------------|--------|------------|-----------|
| TIMING_CHECK | 0x57 | **0** | _(пусто)_ |
| MPQ_CHECK | 0x98 | **1** | `stringIndex(1)` |
| LUA_EVAL_CHECK | 0x8B | **1** | `stringIndex(1)` (стандартный формат) или `length(1) + code[N]` (inline) |
| MEM_CHECK | 0xF3 | **6** | `moduleIdx(1) + address(4) + length(1)` |
| MODULE_CHECK | 0xD9 | **24** | `seed(4) + HMAC-SHA1(20)` |
| DRIVER_CHECK | 0x71 | **25** | `seed(4) + HMAC-SHA1(20) + stringIndex(1)` |
| PAGE_CHECK_A | 0xB2 | **29** | `seed(4) + SHA1(20) + address(4) + length(1)` |
| PAGE_CHECK_B | 0xBF | **29** | `seed(4) + SHA1(20) + address(4) + length(1)` |
| PROC_CHECK | 0x7E | **31** | `seed(4) + SHA1(20) + modIdx(1) + procIdx(1) + offset(4) + length(1)` |

**КРИТИЧЕСКАЯ КОНСТАНТА:** 24-byte Data field

Для всех seed-based checks (DRIVER, MODULE, PAGE, PROC), первые 24 байта всегда:
```
[4 bytes] seed (uint32)
[20 bytes] SHA1 или HMAC-SHA1
```

Это подтверждено во всех emulator implementations через `check->Data.ToByteVector(24, false)`.

### 6.2. Сводная таблица ответов (Response)

| Check Type | Response Size | Pass Value | Fail Value | Дополнительные данные |
|------------|---------------|------------|------------|----------------------|
| **TIMING_CHECK** | **5** | `result != 0x00` | `result == 0x00` | `[result:1][ticks:4]` |
| **DRIVER_CHECK** | **1** | `0xE9` | `!= 0xE9` | Всегда 1 байт |
| **PROC_CHECK** | **1** | `0xE9` | `!= 0xE9` | Всегда 1 байт |
| **MODULE_CHECK** | **1** | `0xE9` | `!= 0xE9` | Всегда 1 байт |
| **PAGE_CHECK_A** | **1** | `0xE9` | `!= 0xE9` | Всегда 1 байт |
| **PAGE_CHECK_B** | **1** | `0xE9` | `!= 0xE9` | Всегда 1 байт |
| **MPQ_CHECK** | **1 + 20** | `0x00 + SHA1[20]` | `!= 0x00` (без SHA1) | Plain SHA1, не HMAC |
| **MEM_CHECK** | **1 + N** | `0x00 + bytes[N]` | `!= 0x00` (без bytes) | N = requested length |
| **LUA_EVAL_CHECK** | **1 + 1 + N** | `0x00` (пусто) | `result_byte + strlen + string` | Длина строки + строка |

**Конвенция 0xE9:**

Байт `0xE9` (опкод x86 JMP) используется как "magic constant" для seed-based checks. Означает:
- DRIVER не найден (pass)
- PROC не захукан (pass)
- MODULE не найден (pass)
- PAGE hash совпал (pass)

Любое другое значение = fail.

**Конвенция 0x00:**

Для MEM_CHECK и MPQ_CHECK:
- `0x00` = успешное чтение, данные следуют
- Non-zero = ошибка, данных нет

---

## 7. Механизмы диспетчеризации проверок

### 7.1. 256-байтная таблица ремаппинга

**Назначение:** Преобразование canonical type byte (0x00–0xFF) в handler index (0x00–0x0A).

**Структура:**
```
uint8_t remapTable[256];
```

**Lookup:**
```c
uint8_t canonicalType = wireType ^ xorByte;
uint8_t handlerIndex = remapTable[canonicalType];
CallHandler(handlerIndex);
```

**Observed distribution:**
- **10 валидных handler indices** (0x00–0x09)
- **1 default handler** (0x0A)
- **~216 entries** map to default (0x0A)

**Default handler behavior:**

Zero-read, zero-write no-op. Архитектурно обязателен, потому что:
- Request format использует variable-length entries
- Unknown type не может безопасно skip N bytes (N неизвестно)
- Единственный безопасный вариант: consume 0 bytes, produce 0 bytes

### 7.2. Цепочки CMP/JCC (линейная диспетчеризация)

**Pattern в 11 из 15 модулей:**

```asm
movzx eax, byte ptr [esi]    ; Read type byte
xor   al, [context+4]        ; XOR decode
cmp   al, 0x57               ; TIMING_CHECK?
je    Handler_Timing
cmp   al, 0xF3               ; MEM_CHECK?
je    Handler_Mem
cmp   al, 0x71               ; DRIVER_CHECK?
je    Handler_Driver
; ...
jmp   Handler_Default        ; 0x0A
```

**Характеристики:**
- Compiler output для sparse case values
- O(N) worst-case (sequential comparison)
- ~54 bytes для 9 types (6 bytes per comparison)
- Density: 9 entries across 156-byte range (0x57–0xF3) = ~5.8%

**Альтернативные паттерны для того же switch:**
- `sub al, imm8` / `jz` chains
- `xor al, key` / `ror al, N` obfuscation

### 7.3. Двоичное дерево поиска (BST)

**Trigger:** Module 3E02C87EB2B3D5D29C5E9626E2B59AE6 и подобные.

**Assembly pattern:**

```asm
movzx eax, byte ptr [esi]
xor   al, [context+4]
cmp   eax, 0x80         ; PIVOT 1 (не реальный type!)
ja    Check_High_Range
cmp   eax, 0x40         ; PIVOT 2
ja    Check_Mid_Range
cmp   eax, 0x10         ; LEAF MATCH
jz    Handler_10
; ...
```

**"Phantom ID" problem:**

Сканеры, ищущие `cmp al, imm8` instructions, ошибочно интерпретируют **pivot nodes** (0x80, 0x40) как валидные type IDs. Это компиляторные константы для bisect search space, не protocol identifiers.

**Решение:** Data-Flow Centric Analysis (см. раздел 8.1).

### 7.4. XOR-якорный сканер

**Цель:** Найти dispatch function без знания конкретного паттерна.

**Anchor pattern:**

```
32 [40-7F, rm≠4] 04
```

**Дизассемблирование:**
- `32 XX 04` = `xor r8, [reg+4]`
- `XX` в диапазоне 0x40–0x7F (исключая rm=4, требующий SIB byte)
- Типичные варианты: `xor al, [ecx+4]`, `xor cl, [edx+4]`

**После XOR instruction:**

```asm
movzx eax, al               ; Zero-extend type byte
; Далее либо:
sub al, MIN_ID / dec / cmp  ; CMP/JCC chain
; Либо:
movzx eax, byte [remap_table + eax]  ; Remap table lookup
jmp [handler_table + eax*4]
```

**Два типа dispatch points:**

1. **Request parser** (dispatch chain) — извлекает real type IDs
   - Содержит CMP/JE/JNE sequences
   - **Полезен для Type ID extraction**

2. **Response builder** (remap table) — 200+ entries, но бесполезен
   - Только remap table reference
   - **НЕ содержит real type IDs напрямую**

**Стратегия:**

XOR-anchored scan → identify request parser → extract CMP immediates.

### 7.5. Полиморфизм Type ID

**КРИТИЧЕСКОЕ:** Check type byte values **НЕ универсальны**.

**Подтвержденные примеры:**

| Module | TIMING_CHECK | LUA_EVAL_CHECK | Источник |
|--------|--------------|----------------|----------|
| 7C4ABC97 | 0x57 | 0x8B | TrinityCore enum |
| DA3BF29E | 0x74 | 0x70 | OwnedCore report |
| 9A95D199 | TBD | TBD | No CMP chain found |

**Философия Blizzard:**

> "Modules are written/compiled in several forms so that reverse engineering is very difficult."

Каждый модуль может использовать собственный набор canonical values. Это **архитектурная особенность**, не баг.

**Последствия для эмуляторов:**

Open-source cores (TrinityCore/AzerothCore/MaNGOS) **hardcode** values из одного reverse-engineered модуля. Если сервер загружает другой модуль, enum values не совпадут, проверки провалятся.

**Решение:** Динамическое извлечение Type IDs из каждого конкретного модуля (см. раздел 8).

---

## 8. Извлечение Type ID из модулей

### 8.1. Метод Data-Flow Centric Analysis (DFCA)

**Концепция "Signature by Consumption":**

Независимо от dispatch mechanism, каждый handler **потребляет фиксированное количество байт** из packet stream. Это инвариант протокола.

**Canonical consumption signatures:**

| Handler | Bytes Consumed | Identification Heuristic |
|---------|----------------|--------------------------|
| TIMING_CHECK | 0 | Empty body; calls `GetTickCount` or `RDTSC` |
| MPQ_CHECK | 1 | Reads 1 byte; calls SFile* functions |
| LUA_EVAL_CHECK | 1 | Reads 1 byte (index); calls `FrameScript::GetText` |
| MEM_CHECK | 6 | Reads 1+4+1; direct memory read |
| MODULE_CHECK | 24 | Reads 4+20; calls `CreateToolhelp32Snapshot` |
| DRIVER_CHECK | 25 | Reads 4+20+1; calls `CreateFileW` or `QueryDosDeviceA` |
| PAGE_CHECK | 29 | Reads 4+20+4+1; calls `VirtualQuery` |
| PROC_CHECK | 31 | Reads 4+20+1+1+4+1; calls `GetModuleHandleA` + `GetProcAddress` |

**Algorithm:**

1. **Locate PacketHandler export** via export table
2. **CFG traversal:** Recursive descent from dispatch point
3. **Leaf node collection:** Functions reachable via conditional jumps
4. **Consumption profiling:** Track `ESI`/`EDI` pointer advancement
   - `LODSB` → +1
   - `LODSD` → +4
   - `ADD ESI, X` → +X
5. **Signature matching:** Map byte count to canonical category
6. **ID extraction:** Back-trace CFG to find `CMP AL, imm8` leading to leaf

**Disambiguate same-size handlers:**

- **PAGE_CHECK_A vs PAGE_CHECK_B** (both 29 bytes):
  - Scan for constant `0x5A4D` (MZ header signature)
  - If present → PAGE_CHECK_B
  - If absent → PAGE_CHECK_A

- **DRIVER_CHECK vs MODULE_CHECK** (25 vs 24 bytes):
  - DRIVER: calls `CreateFileW`, `QueryDosDeviceA`
  - MODULE: calls `CreateToolhelp32Snapshot`, `Module32First`

- **MPQ_CHECK vs LUA_EVAL_CHECK** (both 1 byte):
  - MPQ: calls `SFileOpenFile`, `SFileReadFile`
  - LUA: calls `FrameScript::GetText`

### 8.2. Метод remap cross-reference

**Для модулей БЕЗ dispatch chain** (e.g., 90278079).

**Стратегия:**

Remap table содержит 256 entries, большинство → default (0x0A). Валидные type IDs находятся в **singleton/pair/triplet groups**.

**Algorithm:**

1. Extract remap table (256 bytes)
2. Group entries by handler index:
   ```
   handlers[index] = list of canonical bytes mapping to index
   ```
3. Filter groups: keep только ≤5 entries (singleton through quintuplet)
4. Cross-reference с другими таблицами (если есть shift variants)
5. Intersection дает real type IDs

**Observed в модуле 90278079:**

- Remap-only (no dispatch chain)
- 2 remap tables (shift=0x27)
- 9 types extracted via cross-reference
- All packets use fixed `xorByte = 0x2F`

### 8.3. Метод CFG traversal и Leaf Node analysis

**Расширение DFCA:**

1. **Build complete CFG** from PacketHandler
2. **Identify all terminal nodes** (функции, не branching back)
3. **Profile each terminal:**
   - Track all reads from buffer pointer
   - Track API calls
4. **Classify via decision tree:**
   ```
   IF count == 0 AND calls(GetTickCount) → TIMING
   IF count == 1 AND calls(SFileOpenFile) → MPQ
   IF count == 6 AND direct_memory_read → MEM
   ...
   ```
5. **Extract type bytes** from predecessors в CFG

### 8.4. Z3/SMT solver approach

**Для полностью неизвестных модулей:**

**Problem formulation:**

9 unknown integer variables (размеры типов).

**Constraints из каждого packet:**

```python
from z3 import *
s = Solver()
sizes = {t: Int(f'size_{t}') for t in VALID_TYPE_IDS}

# Domain constraints
for sz in sizes.values():
    s.add(sz >= 0, sz <= 40)

# Packet constraints (пример):
# pos[0] = 0
# pos[i+1] = pos[i] + 1 + sizes[data[pos[i]] ^ xorByte]
# Final: pos[last] == len(data)
```

**Advantages:**

- Global consistency enforcement
- Overdetermined system с multiple packets
- Millisecond решение для 9 unknowns

**Code complexity:** ~100–200 lines Python.

### 8.5. System of linear equations

**Если известны counts:**

```
sum(count_i × size_i) = total_data_length
```

С ≥9 linearly independent packets → Gaussian elimination → O(n³) solution.

**Используется как fast pre-filter для Z3.**

### 8.6. Symbolic execution (angr/Triton/Unicorn)

**Для handlers с conditional reads:**

1. Load module в angr
2. Hook `CDataStore::Read` as SimProcedure
3. Track size arguments
4. Symbolic execution over all paths
5. Determine symbolic relationship: `total_bytes = f(input_data)`

**Применимо к LUA_EVAL_CHECK inline format:**

```
size = 1 + length_byte + string_length
```

Где `string_length` зависит от prior read.

### 8.7. Mac module cross-reference

**Alternative approach:**

Blizzard shipped Mach-O Warden modules для Mac clients **с symbol names**. Cross-reference Mac module's named handlers с Windows module (same logical version) → ground-truth identification без reverse engineering.

**Limitation:** Требует доступа к Mac module того же version.

---

## 9. Детальный анализ каждого типа проверки

### 9.1. TIMING_CHECK (0x57)

#### Назначение

- **Speed hack detection:** Проверка, что `GetTickCount()` не detoured
- **Virtualization detection:** Overhead RDTSC в VM environments
- **Debugging detection:** Time delta между вызовами

#### Request format

```
[type_byte ^ xorByte : 1]
(no additional data)
```

**Total:** 0 bytes после type byte.

#### Response format

```
[result : 1]
[ticks : 4 LE]
```

**Total:** 5 bytes.

**Result byte interpretation:**
- `0x00` = fail (suspicious timing)
- `!= 0x00` = pass

**Ticks:** `GetTickCount()` value (milliseconds since boot).

#### Server-side validation

```cpp
case TIMING_CHECK:
{
    uint8_t result;
    uint32_t ticks;
    buff >> result >> ticks;

    // Compare против server clock с учетом latency
    uint32_t expected = GetTickCount() - latency;
    uint32_t delta = abs(ticks - expected);

    if (delta > THRESHOLD || result == 0x00)
        checkFailed = id;
}
```

**Threshold:** Обычно несколько секунд. Точное значение не публикуется.

#### Механизм детекции

Некоторые модули используют **multi-sample approach:**

1. Record `start_ticks` при init
2. При каждом TIMING_CHECK:
   ```
   current = GetTickCount();
   delta = current - previous;
   ```
3. Analyse `delta` distribution for anomalies

**11-byte hypothesis (advanced):**

Некоторые исследования предполагают расширенный формат:
```
[result:1][ticks:4][delta:4][checkID:2]
```

Но стандартная имплементация — 5 байт.

### 9.2. MEM_CHECK (0xF3)

#### Назначение

Direct memory byte comparison. Используется для проверки:
- Function prologues (inline hook detection)
- Import Address Table (IAT hook detection)
- Critical variables (game state integrity)

#### Request format

```
[type_byte ^ xorByte : 1]
[moduleNameIndex : 1]    // Обычно 0x00 для WoW.exe
[address : 4 LE]
[length : 1]
```

**Total:** 6 bytes.

**ВАЖНО:** MEM_CHECK entries идут в **отдельной секции** (Section 3), **БЕЗ XOR encoding type byte**.

#### Response format

**Success (memory read OK):**
```
[0x00 : 1]
[bytes : length]
```

**Failure (read error):**
```
[non-zero error code : 1]
(no bytes follow)
```

#### Server-side validation

```cpp
case MEM_CHECK:
{
    uint8_t result;
    buff >> result;

    if (result != 0x00) {
        checkFailed = id;  // Read error
        continue;
    }

    // RAW BYTE COMPARISON (не hash!)
    if (memcmp(buff.contents() + buff.rpos(),
               rs->Result.AsByteArray(0, false).get(), rd->Length) != 0) {
        checkFailed = id;  // Mismatch
    }

    buff.rpos(buff.rpos() + rd->Length);
}
```

**Критически:** Сервер делает **bitwise exact comparison**, не SHA1.

#### Database structure

**Table:** `warden_checks`

| Column | Type | Содержимое |
|--------|------|------------|
| id | smallint | Unique check ID |
| type | tinyint | `0xF3` |
| data | text/blob | Hex string: `[moduleIdx:2][address:8][length:2]` |
| result | text/blob | Hex string: expected bytes |
| str | varchar | Module name (обычно пусто для 0x00) |
| comment | varchar | Description |

**Пример:**
```sql
INSERT INTO warden_checks VALUES
(1, 0xF3, '00004010000105', '8BFF558BEC', 'WoW.exe::SendPacket prologue');
```

Означает: Check WoW.exe (module 0) at RVA 0x00401000, read 5 bytes, expect `8B FF 55 8B EC`.

#### Spoofing strategy: Shadow Copy

**Problem:** Если установлен inline hook (JMP), память содержит `E9 XX XX XX XX`.

**Solution:**

1. **Pre-load clean image:**
   ```cpp
   std::vector<uint8_t> cleanImage;
   LoadFileToMemory("C:\\WoW\\WoW.exe", cleanImage);
   ```

2. **RVA → File Offset translation:**
   ```cpp
   uint32_t RvaToFileOffset(uint32_t rva) {
       for (auto& sec : sections) {
           if (rva >= sec.VirtualAddress &&
               rva < sec.VirtualAddress + sec.VirtualSize) {
               uint32_t delta = rva - sec.VirtualAddress;
               return sec.PointerToRawData + delta;
           }
       }
       return 0;  // Error
   }
   ```

3. **Intercept CMSG assembly:**
   ```cpp
   if (check.type == MEM_CHECK) {
       uint32_t fileOffset = RvaToFileOffset(check.address);
       memcpy(response_buffer, &cleanImage[fileOffset], check.length);
   }
   ```

4. **Recalculate checksum:**
   ```cpp
   uint32_t checksum = BuildChecksum(response_buffer, resultLen);
   WriteUInt32LE(packet + 3, checksum);
   ```

**ASLR consideration:**

WoW 3.3.5a обычно загружается по фиксированному адресу `0x00400000`. ASLR не активен. Если включен, необходимо:
```
actual_address = request_RVA + GetModuleHandle(NULL);
```

Но для file offset translation RVA остается правильным.

### 9.3. PAGE_CHECK_A (0xB2) и PAGE_CHECK_B (0xBF)

#### Назначение

- **PAGE_CHECK_A:** SHA1 hash всех executable pages
- **PAGE_CHECK_B:** SHA1 hash только MZ+PE header pages

Используется для детекции крупных модификаций (.text section patches).

#### Request format

```
[type_byte ^ xorByte : 1]
[seed : 4]
[SHA1 : 20]
[address : 4 LE]
[length : 1]
```

**Total:** 29 bytes.

**24-byte Data field:** `seed(4) + SHA1(20)`

#### Response format

```
[result : 1]
```

**Total:** 1 byte.

**Values:**
- `0xE9` = pass (hash matched)
- Any other = fail

#### Client-side algorithm

**PAGE_CHECK_A:**
```cpp
1. Call VirtualQuery(address) to get page info
2. For each page in range:
   - Read page bytes (usually 4KB)
   - Compute HMAC-SHA1(seed, page_bytes)
3. If computed hash == provided SHA1:
   return 0xE9
4. Else:
   return 0x00 (or другой non-0xE9 value)
```

**PAGE_CHECK_B:**
```cpp
1. Read WORD at address (MZ signature check)
2. If word != 0x5A4D:
   return 0xE9 (не PE, skip)
3. Else:
   - Read PE headers
   - Hash только header pages
   - Compare
```

**0x5A4D disambiguation:**

```asm
; Inside PAGE_CHECK_B handler
mov  ax, [esi]        ; Read first 2 bytes
cmp  ax, 0x5A4D       ; 'MZ' signature?
jne  ReturnPass       ; Not PE → 0xE9
; ... continue PE processing
```

Scan for constant `0x5A4D` в handler code → если найден, это PAGE_CHECK_B.

#### Server-side validation

```cpp
case PAGE_CHECK_A:
case PAGE_CHECK_B:
{
    const uint8_t byte = 0xE9;
    if (memcmp(buff.contents() + buff.rpos(), &byte, sizeof(uint8_t)) != 0)
        checkFailed = id;
    buff.rpos(buff.rpos() + 1);
}
```

**Penalty:** Same as other checks (kick/ban configurable).

#### Spoofing considerations

**Problem:** Page hash covers 4096+ bytes. Shadow Copy подход возможен, но:

1. **Large read volume:** Reading 4KB+ из file медленнее
2. **Checksum collision:** Подменить 1 byte в page → полностью другой hash

**Recommendation:**

Избегать PAGE_CHECK targets. Если module устанавливает hooks в .text, ensure:
- Hook pages не являются targets для PAGE_CHECK
- Или использовать trampoline в отдельной RWX page (не в .text)

### 9.4. MPQ_CHECK (0x98)

#### Назначение

Проверка целостности game data files (MPQ archives):
- common.MPQ
- expansion.MPQ
- patch-{letter}.MPQ
- locale-specific files

Детекция модификаций:
- Texture hacks (wallhack через alpha channels)
- Model modifications (collision detection bypass)
- DBC file edits (spell modification)

#### Request format

```
[type_byte ^ xorByte : 1]
[fileNameIndex : 1]
```

**Total:** 1 byte.

Filename берется из string table.

#### Response format

**Success:**
```
[0x00 : 1]
[SHA1 : 20]
```

**Failure (file not found/read error):**
```
[non-zero : 1]
(no SHA1)
```

**Total:** 21 bytes (success) или 1 byte (fail).

#### Client-side algorithm

```cpp
1. Get filename from stringTable[fileNameIndex]
2. Call SFileOpenFileEx(filename, ...)
3. If fail: return error code
4. Get file size via SFileGetFileSize
5. Allocate buffer
6. SFileReadFile(buffer, size)
7. Compute SHA1(buffer, size)  // Plain SHA1, НЕ HMAC
8. Return [0x00][SHA1]
```

**SFile API offsets (Build 12340):**

См. раздел 5.2 (MODULE_INITIALIZE).

#### Server-side validation

```cpp
case MPQ_CHECK:
{
    uint8_t result;
    buff >> result;

    if (result != 0x00) {
        checkFailed = id;  // File error
    } else {
        // Plain SHA1 comparison
        if (memcmp(buff.contents() + buff.rpos(),
                   rs->Result.AsByteArray(0, false), 20) != 0) {
            checkFailed = id;  // Hash mismatch
        }
        buff.rpos(buff.rpos() + 20);
    }
}
```

#### Spoofing strategy: Pre-computed hashes

**Problem:** Если MPQ файлы модифицированы, hash не совпадет.

**Solution:**

1. **Offline preparation:**
   ```bash
   sha1sum common.MPQ
   sha1sum expansion.MPQ
   sha1sum patch-w.MPQ  # Warmane-specific
   ```

2. **Build hash database:**
   ```cpp
   std::map<std::string, uint8_t[20]> cleanHashes = {
       {"Data\\common.MPQ", {0x1A, 0x2B, ...}},
       {"Data\\patch-w.MPQ", {0x3C, 0x4D, ...}}
   };
   ```

3. **Intercept response:**
   ```cpp
   if (check.type == MPQ_CHECK) {
       std::string filename = stringTable[check.stringIndex];
       auto it = cleanHashes.find(filename);
       if (it != cleanHashes.end()) {
           WriteUInt8(buffer, 0x00);
           memcpy(buffer + 1, it->second, 20);
       }
   }
   ```

**Warmane caveat:**

Warmane распространяет собственный клиент с **кастомными MPQ** (HD models из WoD/Legion). "Чистым" hash для них является hash **их версии**, не оригинального Blizzard 3.3.5a.

**КРИТИЧНО:** Использовать hashes от Warmane client, не vanilla.

### 9.5. LUA_EVAL_CHECK (0x8B)

#### Назначение

Execution Lua code на клиенте и return результат. Используется для:
- Проверки protected variables
- Детекции unlock addon
- Проверки game state inconsistencies

#### Request format (два варианта)

**Стандартный (string table index):**
```
[type_byte ^ xorByte : 1]
[stringIndex : 1]
```

**Enhanced AzerothCore (inline):**
```
[type_byte ^ xorByte : 1]
[totalLength : 1]
[prefix : N1]
[checkExpr : N2]
[midfix : N3]
[checkID : 4]
[postfix : N4]
```

Max length: 170 bytes.

#### Response format

**Пустой результат (pass):**
```
[0x00 : 1]
```

**Результат присутствует:**
```
[result_byte : 1]
[strlen : 1]
[string : strlen]
```

#### Client-side algorithm

```cpp
1. Get Lua code from stringTable[stringIndex]
   OR parse inline Lua code
2. Call FrameScript::Execute(code)  // Lua VM execution
3. If result empty:
   return [0x00]
4. Else:
   result_string = GetLuaResult()
   return [result_byte][strlen][result_string]
```

**FrameScript::GetText offset:** `0x00819D40` (Build 12340)

#### Server-side validation

```cpp
case LUA_EVAL_CHECK:
{
    uint8_t result;
    buff >> result;

    if (result == 0x00) {
        // Empty result (expected для многих checks)
        // Pass
    } else {
        uint8_t strlen;
        std::string str;
        buff >> strlen;
        str.resize(strlen);
        buff.read((uint8*)str.data(), strlen);

        // Validate result против expected
        if (str != rs->str)
            checkFailed = id;
    }
}
```

#### Typical checks

**Примеры Lua code:**

```lua
-- Проверка unlock addon
return GetCVar("ScriptErrors")

-- Проверка protected function call
return (pcall(UseAction, 1) and "1" or "0")

-- Проверка specific addon
return IsAddOnLoaded("PQR") and "1" or "0"
```

#### Spoofing strategy

**Passive approach:** Всегда return `[0x00]` (empty result).

**Active approach:**

1. Parse Lua code
2. Detect suspicious checks (addon names, protected calls)
3. If suspicious: return empty
4. If benign: execute honestly

**Risk:** Lua execution может trigger side effects. Safer to spoof.

### 9.6. DRIVER_CHECK (0x71)

#### Назначение

Детекция kernel drivers используемых для:
- VM environments (VMware, VirtualBox, Parallels)
- Kernel-mode cheats (memory manipulation drivers)
- Debugging tools (WinDbg kernel debugger)

#### Request format

```
[type_byte ^ xorByte : 1]
[seed : 4]
[HMAC-SHA1 : 20]
[driverNameIndex : 1]
```

**Total:** 25 bytes.

#### Response format

```
[result : 1]
```

**Values:**
- `0xE9` = driver NOT found (pass)
- Any other = driver found (fail)

#### Client-side algorithm

```cpp
1. Get driverName from stringTable[driverNameIndex]
2. Construct device path: "\\.\\" + driverName
3. Call CreateFileW(devicePath, ...)
4. If handle == INVALID_HANDLE_VALUE:
   return 0xE9  // Not found
5. Else:
   CloseHandle(handle)
   return 0x00  // Found (fail)
```

**Alternative API:** `NtQuerySystemInformation(SystemModuleInformation)` для enum loaded kernel modules.

#### Server-side validation

```cpp
case DRIVER_CHECK:
{
    const uint8_t byte = 0xE9;
    if (memcmp(buff.contents() + buff.rpos(), &byte, sizeof(uint8_t)) != 0)
        checkFailed = id;  // Driver WAS found
    buff.rpos(buff.rpos() + 1);
}
```

#### Типичные drivers

**VM detection:**
- `vmmemctl` — VMware memory control
- `vboxguest` — VirtualBox guest additions
- `VBoxSF` — VirtualBox shared folders
- `vmci` — VMware Communication Interface
- `prl_fs` — Parallels file system

**Cheat drivers:**
- Custom kernel drivers для direct physical memory access
- Driver names обычно randomized

#### Spoofing strategy

**Trivial:** Всегда return `0xE9`.

```cpp
if (check.type == DRIVER_CHECK) {
    // Skip data parsing
    ParseAndSkip(check);
    WriteUInt8(buffer, 0xE9);
}
```

**Safety:** Все server implementations expect 0xE9 for clean system. Нет "reverse checks" expecting driver presence.

### 9.7. MODULE_CHECK (0xD9)

#### Назначение

Детекция injected DLLs в address space процесса. Ищет:
- CheatEngine.dll
- Известные bot frameworks
- Custom injection DLLs

#### Request format

```
[type_byte ^ xorByte : 1]
[seed : 4]
[HMAC-SHA1 : 20]
```

**Total:** 24 bytes.

**ВАЖНАЯ ОСОБЕННОСТЬ:** Module name **НЕ ПЕРЕДАЕТСЯ** в пакете. Сервер генерирует HMAC, клиент **iterates all loaded modules** и проверяет каждую.

#### Response format

```
[result : 1]
```

**Values:**
- `0xE9` = NOT found (pass)
- Other = found (fail)

#### Client-side algorithm

```cpp
1. Call CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId())
2. For each module via Module32First/Module32Next:
   - Get module name (e.g., "CheatEngine.dll")
   - Compute HMAC-SHA1(seed, moduleName)
   - If computed HMAC == provided HMAC:
     return 0x00  // FOUND (fail)
3. If no match found:
   return 0xE9  // NOT FOUND (pass)
```

**HMAC-SHA1 generation (server-side):**

```cpp
case MODULE_CHECK:
{
    uint32_t seed = rand32();
    buff << uint32(seed);

    HmacHash hmac(4, (uint8*)&seed);  // 4-byte key
    hmac.UpdateData(moduleName);      // Target string
    hmac.Finalize();

    buff.append(hmac.GetDigest(), 20);
}
```

#### Server-side validation

Same as DRIVER_CHECK — expects `0xE9`.

#### Index desynchronization bug

**КРИТИЧЕСКАЯ ОСОБЕННОСТЬ 3.3.5a:**

MODULE_CHECK **НЕ ЧИТАЕТ** string index из packet stream, хотя module name **ПРИСУТСТВУЕТ** в string table.

**Код сервера:**
```cpp
case MODULE_CHECK:
{
    // NOTE: НЕТ читки stringIndex!
    buff << seed;
    buff << HMAC;
    // String добавлен в string table, но индекс НЕ записывается
}
```

**Последствие:** При парсинге последовательности проверок, string index counter **НЕ инкрементируется** для MODULE_CHECK. Следующая DRIVER_CHECK, ожидающая "index 1", получит "index 0".

**Spoofing implementation ДОЛЖНА эмулировать этот баг** для correct string table synchronization.

#### Spoofing strategy

**Trivial:** Return `0xE9`.

```cpp
if (check.type == MODULE_CHECK) {
    packet.Skip(24);  // seed + HMAC
    WriteUInt8(buffer, 0xE9);
}
```

**Alternative (стелс):**

1. Rename injected DLL to benign name (e.g., `d3d9.dll`)
2. Server unlikely to check legitimate DLLs (too many false positives)

### 9.8. PROC_CHECK (0x7E)

#### Назначение

Детекция API hooking на function-level:
- Inline hooks (JMP at prologue)
- IAT hooks
- VTable hooks

Проверяет integrity function prologues через HMAC comparison.

#### Request format

```
[type_byte ^ xorByte : 1]
[seed : 4]
[HMAC-SHA1 : 20]
[moduleNameIndex : 1]
[procNameIndex : 1]
[offset : 4 LE]
[length : 1]
```

**Total:** 31 bytes.

**Heaviest check type.**

#### Response format

```
[result : 1]
```

**Values:**
- `0xE9` = prologue clean (pass)
- Other = modified (fail)

#### Client-side algorithm

```cpp
1. moduleName = stringTable[moduleNameIndex]
2. procName = stringTable[procNameIndex]
3. hModule = GetModuleHandleA(moduleName)
4. If hModule == NULL: return error
5. funcPtr = GetProcAddress(hModule, procName)
6. If funcPtr == NULL: return error
7. target = (uint8*)funcPtr + offset
8. memcpy(buffer, target, length)
9. Compute HMAC-SHA1(seed, buffer)
10. If computed == provided HMAC:
    return 0xE9  // Clean
11. Else:
    return 0x00  // Modified
```

**Offset usage:**

Обычно `offset = 0` (prologue start). Но может быть ≠0 для проверки mid-function bytes.

**Length:**

Обычно 5–12 bytes (размер стандартного hook: 5-byte JMP).

#### Server-side validation

Same as DRIVER_CHECK.

#### Spoofing strategy: Shadow Copy

**Problem:** Если установлен inline hook, память содержит:
```
E9 XX XX XX XX  ; JMP rel32
```

Вместо оригинального prologue:
```
55              ; PUSH EBP
8B EC           ; MOV EBP, ESP
```

**Solution:**

1. **Pre-load clean modules:**
   ```cpp
   HANDLE hFile = CreateFileA("C:\\WoW\\WoW.exe", ...);
   HANDLE hMap = CreateFileMappingA(hFile, NULL, PAGE_READONLY, 0, 0, NULL);
   void* cleanBase = MapViewOfFile(hMap, FILE_MAP_READ, 0, 0, 0);
   ```

2. **Translate RVA:**
   ```cpp
   funcPtr = GetProcAddress(GetModuleHandle(moduleName), procName);
   rva = funcPtr - GetModuleHandle(moduleName);
   cleanPtr = cleanBase + rva;
   ```

3. **Read clean bytes:**
   ```cpp
   memcpy(buffer, (uint8*)cleanPtr + offset, length);
   ```

4. **Return HMAC or just 0xE9:**

   Если check **local comparison** (computed HMAC == provided):
   - HMAC already computed client-side
   - Return только result byte (0xE9 или 0x00)

   Если check отправляет raw bytes на сервер для сравнения (редкий вариант):
   - Return `[0x00][clean_bytes]`

**В стандартных implementations:** PROC_CHECK делает local HMAC comparison, возвращает 1 byte.

#### Typical targets

**Windows API:**
- `OpenProcess` (kernel32.dll)
- `ReadProcessMemory` (kernel32.dll)
- `NtReadVirtualMemory` (ntdll.dll)
- `NtQueryInformationProcess` (ntdll.dll)
- `NtSetInformationThread` (ntdll.dll)

**Game-specific:**
- `Lua_DoString` (lua51.dll)
- `FrameScript::Execute` (WoW.exe)
- `NetClient::Send` (WoW.exe)
- `EndScene` (d3d9.dll — for overlays)

#### Status in emulators

**PROC_CHECK is COMMENTED OUT** in all examined codebases (TrinityCore, AzerothCore, MaNGOS).

```cpp
/* PROC_CHECK code commented:
case PROC_CHECK:
{
    // ... code exists but disabled
}
*/
```

**Reason:** High false-positive rate (legitimate software hooks API).

**Consequence:** Spoofing может не требоваться if server doesn't send PROC_CHECK.

---

## 10. Спуфинг MEM_CHECK: Анализ вариантов

### 10.1. Вариант A: Модификация plaintext до RC4 (РЕКОМЕНДУЕМЫЙ)

**Архитектура:**

Hook RC4 PRGA function inside Warden module **before encryption** of CMSG_WARDEN_DATA.

**Преимущества:**

1. **Full plaintext access** — видим структуру пакета
2. **Surgical modification** — заменяем только нужные bytes
3. **Checksum recalculation** — пересчитываем SHA1 XOR-fold
4. **RC4 sync guaranteed** — keystream не зависит от plaintext

**Алгоритм:**

```cpp
void HookRC4Encrypt(uint8* data, uint32 len, RC4Context* ctx) {
    // 1. Parse packet
    if (data[0] != 0x02) {
        // Not CHEAT_CHECKS_RESULT
        return OriginalRC4(data, len, ctx);
    }

    // 2. Parse structure
    uint16 resultLen = *(uint16*)(data + 1);
    uint32 checksum = *(uint32*)(data + 3);
    uint8* results = data + 7;

    // 3. Walk results section
    uint32 offset = 0;
    for (auto& check : pendingChecks) {
        if (check.type == MEM_CHECK && check.status == 0x00) {
            uint32 addr = check.address;
            uint32 len = check.length;

            // Check if overlaps any hook
            for (auto& hook : installedHooks) {
                if (AddressOverlaps(addr, len, hook.address, hook.length)) {
                    // Replace with clean bytes
                    memcpy(results + offset + 1, hook.originalBytes, len);
                }
            }
        }
        offset += GetResponseSize(check);
    }

    // 4. Recalculate checksum
    uint32 newChecksum = SHA1_XOR_Fold(results, resultLen);
    *(uint32*)(data + 3) = newChecksum;

    // 5. Encrypt modified data
    OriginalRC4(data, len, ctx);
}
```

**Hook installation point:**

Pattern scan for RC4 PRGA inside Warden module:
```
55 8B EC  ; push ebp; mov ebp, esp
```

Multiple RC4 functions may exist (main thread, module thread). Hook all viable candidates.

**Deferred installation:**

Install hooks **after HASH_REQUEST** is processed. Installing before causes integrity hash mismatch → re-key desync.

### 10.2. Вариант B: XOR-patch ciphertext

**Formula:**

```
new_cipher = old_cipher ⊕ old_plain ⊕ new_plain
```

**Требования:**

1. Capture old plaintext before encryption
2. Capture old ciphertext after encryption
3. Compute delta: `delta = old_plain ⊕ new_plain`
4. Apply: `new_cipher = old_cipher ⊕ delta`

**Преимущества:**

- No need to find RC4 function inside module
- Hook point in WoW.exe (stable address)

**Недостатки:**

- Requires **two interception points** (before + after RC4)
- More complex state management
- Checksum also needs XOR-patching (4 bytes)

**Вывод:** Если уже есть plaintext access, Variant A проще.

### 10.3. Вариант C: Modify internal buffer

**Idea:** Find Warden module's internal result buffer **between assembly and encryption**.

**Недостатки:**

- Module internals undocumented
- Changes between module versions
- Requires deep RE of each module
- Very fragile

**Вывод:** Не рекомендуется.

### 10.4. Вариант D: Temporary unhook

**Idea:**

1. Before Warden scan: restore original bytes
2. Let scan execute
3. After scan: reinstall hooks

**Преимущества:**

- No packet modification
- Checksum automatically valid

**Недостатки:**

- **Race conditions:**
  - If hooked code called during unhook window → crash
  - Partially-written instructions → crash if interrupt fires

- **Atomicity issues:**
  - 5-byte JMP write not atomic on x86
  - Requires `VirtualProtect` + `FlushInstructionCache`

- **Timing overhead:**
  - Detectable via TIMING_CHECK

**Вывод:** Не рекомендуется из-за race conditions.

### 10.5. Verdict

**Variant A (modify plaintext before RC4) is optimal:**

- Strongest correctness guarantees
- Single interception point
- Full plaintext access
- No race conditions
- RC4-safe by construction

---

## 11. Спуфинг DRIVER_CHECK, MPQ_CHECK и PROC_CHECK

### 11.1. DRIVER_CHECK: всегда 0xE9

```cpp
void SpoofDriverCheck(ByteBuffer& result) {
    result.WriteUInt8(0xE9);
}
```

**Обоснование:**

Все server implementations expect `0xE9` for clean system. Нет reverse checks.

### 11.2. MPQ_CHECK: предвычисленные SHA1 хеши

```cpp
std::map<std::string, uint8_t[20]> cleanHashes;

void InitMpqHashes() {
    // Pre-compute offline
    cleanHashes["Data\\common.MPQ"] = {0x1A, 0x2B, ...};
    cleanHashes["Data\\expansion.MPQ"] = {0x3C, 0x4D, ...};
    // Warmane-specific
    cleanHashes["Data\\patch-w.MPQ"] = {0x5E, 0x6F, ...};
}

void SpoofMpqCheck(const std::string& filename, ByteBuffer& result) {
    auto it = cleanHashes.find(filename);
    if (it != cleanHashes.end()) {
        result.WriteUInt8(0x00);
        result.append(it->second, 20);
    } else {
        // Unknown file, report error
        result.WriteUInt8(0x01);
    }
}
```

**Генерация hashes:**

```bash
# Linux
sha1sum Data/*.MPQ > mpq_hashes.txt

# Windows
certutil -hashfile "Data\common.MPQ" SHA1
```

### 11.3. PROC_CHECK: Shadow Copy

```cpp
class ShadowImageManager {
    std::map<std::string, MappedFile> modules;

public:
    void LoadModule(const std::string& path) {
        HANDLE hFile = CreateFileA(path.c_str(), GENERIC_READ,
                                    FILE_SHARE_READ, NULL, OPEN_EXISTING,
                                    FILE_ATTRIBUTE_NORMAL, NULL);
        HANDLE hMap = CreateFileMappingA(hFile, NULL, PAGE_READONLY, 0, 0, NULL);
        void* base = MapViewOfFile(hMap, FILE_MAP_READ, 0, 0, 0);

        modules[path] = {hFile, hMap, base};
    }

    void ReadCleanBytes(const std::string& moduleName,
                       const std::string& procName,
                       uint32_t offset, uint32_t length,
                       uint8_t* output) {
        // 1. Get runtime address
        HMODULE hMod = GetModuleHandleA(moduleName.c_str());
        void* proc = GetProcAddress(hMod, procName.c_str());
        uint32_t rva = (uint32_t)proc - (uint32_t)hMod;

        // 2. Translate to file offset
        auto& mapped = modules[moduleName];
        uint32_t fileOffset = RvaToFileOffset(mapped.base, rva);

        // 3. Read clean bytes
        memcpy(output, (uint8*)mapped.base + fileOffset + offset, length);
    }
};

void SpoofProcCheck(const ProcCheckRequest& req, ByteBuffer& result) {
    uint8_t cleanBytes[256];
    shadowMgr.ReadCleanBytes(req.moduleName, req.procName,
                            req.offset, req.length, cleanBytes);

    // PROC_CHECK uses local HMAC comparison
    // Just return 0xE9 (pass)
    result.WriteUInt8(0xE9);
}
```

#### RVA to File Offset translation

```cpp
uint32_t RvaToFileOffset(void* baseAddr, uint32_t rva) {
    IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)baseAddr;
    IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)((uint8*)baseAddr + dos->e_lfanew);
    IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);

    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++) {
        if (rva >= sec[i].VirtualAddress &&
            rva < sec[i].VirtualAddress + sec[i].Misc.VirtualSize) {
            uint32_t delta = rva - sec[i].VirtualAddress;
            return sec[i].PointerToRawData + delta;
        }
    }

    return 0;  // Error: RVA not in any section
}
```

---

## 12. Методология Shadow Copy

### 12.1. Загрузка чистого образа с диска

```cpp
class ShadowImage {
private:
    std::vector<uint8_t> fileBuffer;
    std::vector<SectionInfo> sections;
    IMAGE_NT_HEADERS* ntHeaders;

public:
    bool Initialize(const std::wstring& path) {
        // 1. Read entire file
        std::ifstream file(path, std::ios::binary | std::ios::ate);
        if (!file.is_open()) return false;

        size_t fileSize = file.tellg();
        file.seekg(0);
        fileBuffer.resize(fileSize);
        file.read((char*)fileBuffer.data(), fileSize);

        // 2. Parse PE headers
        IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)fileBuffer.data();
        if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;

        ntHeaders = (IMAGE_NT_HEADERS*)(fileBuffer.data() + dos->e_lfanew);
        if (ntHeaders->Signature != IMAGE_NT_SIGNATURE) return false;

        // 3. Extract section info
        IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(ntHeaders);
        for (int i = 0; i < ntHeaders->FileHeader.NumberOfSections; i++) {
            SectionInfo info;
            info.virtualAddr = sec[i].VirtualAddress;
            info.virtualSize = sec[i].Misc.VirtualSize;
            info.rawOffset = sec[i].PointerToRawData;
            sections.push_back(info);
        }

        return true;
    }

    bool ReadBytes(uint32_t rva, size_t length, std::vector<uint8_t>& out) {
        for (const auto& sec : sections) {
            if (rva >= sec.virtualAddr &&
                rva < sec.virtualAddr + sec.virtualSize) {
                uint32_t offsetInSec = rva - sec.virtualAddr;
                uint32_t fileOffset = sec.rawOffset + offsetInSec;

                if (fileOffset + length > fileBuffer.size())
                    return false;

                out.resize(length);
                memcpy(out.data(), &fileBuffer[fileOffset], length);
                return true;
            }
        }
        return false;
    }
};
```

### 12.2. Обработка релокаций и ASLR

**WoW 3.3.5a:**

- Fixed base: `0x00400000`
- ASLR обычно выключен
- `IMAGE_DLLCHARACTERISTICS_DYNAMIC_BASE` флаг отсутствует

**Проверка:**

```cpp
bool IsASLREnabled(IMAGE_NT_HEADERS* nt) {
    return (nt->OptionalHeader.DllCharacteristics &
            IMAGE_DLLCHARACTERISTICS_DYNAMIC_BASE) != 0;
}
```

**Если ASLR включен:**

```cpp
uint32_t actualBase = (uint32_t)GetModuleHandle(NULL);
uint32_t preferredBase = ntHeaders->OptionalHeader.ImageBase;
int32_t delta = actualBase - preferredBase;

// При чтении: RVA остается правильным
// При записи адресов: нужна корректировка
```

Но для Shadow Copy (только чтение), ASLR не влияет на file offset translation.

### 12.3. Производительность

**Загрузка WoW.exe (~12 MB) в память:**

- Время: ~50 ms (HDD), ~10 ms (SSD)
- Делается один раз при DLL injection

**Чтение из std::vector:**

- L1 cache hit: ~4 cycles (~1 ns)
- Неотличимо от чтения из процесса

**Timing checks не детектируют** этот подход.

---

## 13. RC4 хуки в модуле Warden

### 13.1. Session header cipher (ARC4::Process @ 0x00774EA0)

**ВАЖНО:** Это **НЕ** Warden payload cipher!

Эта функция шифрует **packet headers:**

- CMSG: `[size:2][opcode:4]` = 6 bytes
- SMSG: `[size:2][opcode:2]` = 4 bytes

**Используется для:**

- Шифрование заголовков всех WoW packets
- Независимый контекст от Warden RC4

**Во время Warden handler:**

- Call#1 (len=6, main thread) = encrypting CMSG_WARDEN_DATA header
- Module thread также использует для small operations (len=2)

**НЕ используется для Warden payload** — тот шифруется отдельным RC4 inside module.

### 13.2. Warden payload RC4 inside module blob

**Два подхода:**

#### Подход 1: S-box cloning (warden_rc4.cpp)

**Сканирование MEM_PRIVATE regions для RC4 S-box:**

```cpp
// RC4 S-box layout 1: [S[256]][i][j]
bool IsRC4_Uint8_Layout1(uint8* addr) {
    // Check permutation property
    uint32 counts[256] = {0};
    for (int i = 0; i < 256; i++)
        counts[addr[i]]++;
    for (int i = 0; i < 256; i++)
        if (counts[i] != 1) return false;
    return true;
}

// RC4 S-box layout 2: [i][j][S[256]]
bool IsRC4_Uint8_Layout2(uint8* addr) {
    return IsRC4_Uint8_Layout1(addr + 2);
}

void ScanForRC4States() {
    MEMORY_BASIC_INFORMATION mbi;
    for (uint8* addr = 0; ; addr += mbi.RegionSize) {
        if (!VirtualQuery(addr, &mbi, sizeof(mbi))) break;
        if (mbi.State != MEM_COMMIT) continue;
        if (mbi.Type != MEM_PRIVATE) continue;  // CRITICAL
        if (mbi.RegionSize > 64*1024*1024) continue;  // Skip huge regions

        for (size_t i = 0; i < mbi.RegionSize - 258; i++) {
            if (IsRC4_Uint8_Layout1(addr + i)) {
                CandidateStates.push_back({addr + i, LAYOUT_1});
            }
            if (IsRC4_Uint8_Layout2(addr + i)) {
                CandidateStates.push_back({addr + i, LAYOUT_2});
            }
        }
    }
}
```

**Usage:**

1. WardenPreHandler: Clone all candidate states
2. SendPacket hook: Try each clone to decrypt CMSG
3. Structural validation of decrypted packet

**Candidates observed:** ~40 (20 unique addresses × 2 layouts)

**Session cipher @ 0x0471fcf8** (observed)

**Warden S-boxes @ 0x00b2d728, 0x00b2d828** (consecutive pair = decrypt/encrypt)

#### Подход 2: Internal hook (warden_rc4_hook.cpp)

**Pattern для RC4 PRGA:**

```
55 8B EC  ; push ebp; mov ebp, esp
```

Но register choices vary. Нужен более robust scanner.

**Scanner strategy:**

1. Find ALL viable clusters (≥2 matches with offsets +0x100/+0x101)
2. Group by 120-byte gaps
3. Install hooks на up to 4 functions simultaneously

**Hooks:**

```cpp
void __declspec(naked) HookedRC4Naked_0() {
    __asm {
        pushad
        pushfd

        push [ebp+12]   ; len (может быть другой порядок)
        push [ebp+8]    ; data
        push ecx        ; context (или другой регистр)
        call HookedRC4Handler

        popfd
        popad
        jmp [g_trampolines[0]]
    }
}

void HookedRC4Handler(void* ctx, void* data, uint32 len) {
    // 1. Detect convention
    if (!g_conventionDetected[hookIndex]) {
        DetectConvention(ctx, data, len);
    }

    // 2. If CMSG encryption
    if (IsWardenCmsg(data, len)) {
        // Save plaintext before encryption
        ConsumePlaintext(data, len);
    }

    // 3. Call original (via trampoline)
    CallOriginal(ctx, data, len);
}
```

**Convention detection:**

6 поддерживаемых вариантов:

1. ECX=ctx, stk1=data, stk2=len
2. ECX=ctx, stk1=len, stk2=data
3. EDX=ctx, stk1=data, stk2=len
4. ... (EBX, ESI, EDI, EAX variants)

**Deferred installation:**

```cpp
void InstallRC4Hooks() {
    // MUST be called AFTER HASH_REQUEST processed
    // Otherwise module integrity hash fails
    for (auto& candidate : rc4Candidates) {
        MH_CreateHook(candidate.address, HookedRC4Naked_X, ...);
        MH_EnableHook(candidate.address);
    }
}
```

**Removal:**

```cpp
void RemoveRC4Hooks() {
    // On MODULE_USE or Shutdown
    for (auto& hook : installedHooks) {
        MH_DisableHook(hook.address);
        MH_RemoveHook(hook.address);
    }
}
```

**False positive handling:**

Некоторые modules имеют ложные срабатывания:

- Hook#1: len=0 (no-op PRGA)
- Hook#2: KSA (key setup, identity init loop)
- Hook#3: mid-function epilogue (stores i/j only)

**Решение:** S-box cloning всегда запускается как fallback.

### 13.3. CMSG format confirmed

**Структура CHEAT_CHECKS_RESULT:**

```
[opcode : 1 = 0x02]
[resultLen : 2 LE]
[checksum : 4]
[results : resultLen]
```

**Пустой request (0 checks):**

```
02 00 00 8C BF 48 95
```

- opcode = 0x02
- resultLen = 0x0000 (no results)
- checksum = 0x9548BF8C (SHA1 XOR-fold of empty buffer)

**TIMING_CHECK (5 bytes):**

```
[flag : 1]
[ticks : 4 LE]
```

Flag usually `0x01` для pass.

**LUA_EVAL_CHECK (variable):**

```
[result_byte : 1]
[strlen : 1]
[string : strlen]
```

---

## 14. Реализация на стороне сервера

### 14.1. warden_checks database table

**Schema (TrinityCore/AzerothCore):**

```sql
CREATE TABLE `warden_checks` (
  `id` smallint(5) unsigned NOT NULL,
  `type` tinyint(3) unsigned DEFAULT NULL,
  `data` text,
  `str` text,
  `address` int(10) unsigned DEFAULT NULL,
  `length` tinyint(3) unsigned DEFAULT NULL,
  `result` text,
  `comment` text,
  PRIMARY KEY (`id`)
);
```

**Пример entries:**

```sql
-- MEM_CHECK
INSERT INTO warden_checks VALUES
(1, 0xF3, '', '', 0x00401000, 5, '8BFF558BEC', 'WoW.exe prologue');

-- DRIVER_CHECK
INSERT INTO warden_checks VALUES
(2, 0x71, '...24-byte seed+SHA1...', 'vmmemctl', 0, 0, '', 'VMware driver');

-- MPQ_CHECK
INSERT INTO warden_checks VALUES
(3, 0x98, '', 'Data\\common.MPQ', 0, 0, '1A2B3C...20-byte SHA1...', 'MPQ integrity');
```

### 14.2. Check selection and pools

**AzerothCore categories:**

```cpp
enum WardenCheckCategory {
    WARDEN_CHECK_MEM_TYPE = 0,     // MEM_CHECK
    WARDEN_CHECK_LUA_TYPE = 1,     // LUA_EVAL_CHECK
    WARDEN_CHECK_OTHER_TYPE = 2    // All others
};
```

**Selection algorithm:**

```cpp
void RequestChecks() {
    // TIMING always first
    _CurrentChecks.push_back(TIMING_CHECK_ID);

    // 3 MEM_CHECKs (configurable)
    for (int i = 0; i < 3; i++) {
        uint16 id = SelectRandom(_ChecksTodo[WARDEN_CHECK_MEM_TYPE]);
        _CurrentChecks.push_back(id);
    }

    // 7 OTHER checks (PAGE, DRIVER, MODULE, etc.)
    for (int i = 0; i < 7; i++) {
        uint16 id = SelectRandom(_ChecksTodo[WARDEN_CHECK_OTHER_TYPE]);
        _CurrentChecks.push_back(id);
    }

    // Assemble packet
    BuildChecksRequest();
}
```

**Round-robin shuffle:**

Each pool (`_ChecksTodo[category]`) содержит все check IDs этой категории. Выбираются random without replacement. Когда pool опустошается, refill.

**Config options:**

```
Warden.NumMemChecks = 3
Warden.NumOtherChecks = 7
Warden.ClientCheckHoldOff = 30  # Seconds between cycles
```

### 14.3. Penalty system

**WardenActions enum:**

```cpp
enum WardenActions {
    WARDEN_ACTION_LOG = 0,   // Log only
    WARDEN_ACTION_KICK = 1,  // Disconnect
    WARDEN_ACTION_BAN = 2    // Ban account
};
```

**Config:**

```
Warden.ClientCheckFailAction = 0  # Default: log only
Warden.BanDuration = 86400        # 24 hours (0 = permanent)
```

**Per-check override:**

**Table:** `warden_action` (characters DB)

```sql
CREATE TABLE `warden_action` (
  `wardenId` smallint(5) unsigned NOT NULL,
  `action` tinyint(3) unsigned DEFAULT NULL
);
```

**Logic:**

```cpp
if (checkFailed > 0) {
    WardenCheck* check = sWardenCheckMgr->GetCheck(checkFailed);
    uint8 action = GetActionForCheck(checkFailed);  // Override or default

    switch (action) {
        case WARDEN_ACTION_LOG:
            LOG_WARN("Warden check %u failed for %s", checkFailed, GetPlayerName());
            break;
        case WARDEN_ACTION_KICK:
            GetSession()->KickPlayer();
            break;
        case WARDEN_ACTION_BAN:
            BanAccount(Warden.BanDuration);
            break;
    }
}
```

**Timeout:**

```
Warden.ClientResponseDelay = 600  # 10 minutes
```

Если ответ не получен за это время → kick (независимо от fail action).

### 14.4. AzerothCore interrupt handling

**Problem:** Если `RequestChecks()` вызывается пока response pending, создается race condition.

**Solution:**

```cpp
bool _interrupted = false;

void RequestChecks() {
    if (_dataSent) {
        // Response still pending
        _interrupted = true;
        return;
    }

    // ... normal logic
    _dataSent = true;
}

void HandleData(ByteBuffer& buff) {
    // ... parse response

    if (checkFailed > 0 && !_interrupted) {
        // Apply penalty only if not interrupted
        ApplyPenalty(checkFailed);
    }

    _dataSent = false;
    _interrupted = false;
}
```

Prevents false positives from server-side race conditions.

### 14.5. Warmane "Sentinel" extensions

**Официально не документировано**, но из community reports:

**Дополнительные механизмы:**

1. **Lua function unlock monitoring**
   - Детекция когда protected functions становятся callable
   - Использует `SendAddonMessage` как backchannel (3.3.5a Warden не передает Lua results natively)

2. **DBC alteration detection**
   - Проверка game data файлов (Spell.dbc, Item.dbc)
   - Расширенные MPQ checks

3. **Memory edit persistence tracking**
   - "Once you try a cheat, your memory remains edited for the session"
   - Implies in-memory state tracking beyond single checks

4. **DLL injection detection**
   - Extended MODULE_CHECK coverage
   - Possibly heuristic analysis of module load patterns

**Ban policy:**

- First offense: 13–15 minute suspension
- Repeat violations: 30 days
- Permanent bans for severe cases
- Public announcements of banned players

**No appeal policy** — bans not reversed.

**Custom check definitions:**

Warmane uses heavily customized `warden_checks` database, different from public cores.

---

## 15. Исходный код и репозитории

### 15.1. Server-side emulators

| Repository | Branch | Path | Описание |
|------------|--------|------|----------|
| TrinityCore/TrinityCore | 3.3.5 | `src/server/game/Warden/` | Original implementation |
| azerothcore/azerothcore-wotlk | master | `src/server/game/Warden/` | Enhanced с pool categories |
| mangostwo/server | master | `src/game/Warden/` | Includes Mac support |
| mangoszero/server | master | `src/game/Warden/` | State machine с 7 states |
| cmangos/mangos-classic | master | `src/anticheat/Warden/` | Struct definitions |

**Key files:**

- `Warden.h` — Enum definitions, state machine
- `Warden.cpp` — Base class, crypto, state transitions
- `WardenWin.cpp` — Windows-specific packet assembly
- `WardenMac.cpp` — Mac-specific (symbols preserved)
- `WardenCheckMgr.h/cpp` — Database loading, check selection

### 15.2. Client-side research tools

| Repository | Language | Описание |
|------------|----------|----------|
| namreeb/WardenSigning | C++ | RSA verification, 72 sniffed modules |
| vmangos/warden_modules | Binary | `.bin` + `.key` files для vanilla |
| xakepru/x14.08-coverstory-blizzard | C/C++ | Client-side interception (HackMag article) |
| tomrus88/WoWTools | C# | Packet parsers, XOR key logic |

### 15.3. Python analysis scripts

**Location:** `wotlk/docs/` (this project)

| Script | Назначение |
|--------|-----------|
| `find_request_parsers.py` | XOR-anchored scanner, dispatch chain extraction |
| `extract_all_types.py` | Batch processing всех модулей, Type ID collection |
| `group_sizes.py` | Статистический анализ размеров данных |
| `investigate_missing.py` | Debug tool for missing Type IDs |

**Runtime base detection:**

Scripts используют stable signature (`56 57 FC...`) для поиска module base в memory dumps.

---

## 16. Заключение и рекомендации

### 16.1. Архитектурные выводы

**Три ключевых принципа Warden 3.3.5a:**

1. **Полиморфизм Type ID** — каждый модуль использует собственные canonical values. Универсального статического анализа не существует без per-module extraction.

2. **RC4 persistent state** — synchronization критична. Любое изменение byte count в packet вызывает desync → disconnect. Content-only modifications безопасны.

3. **Client-side trust model** — фундаментальная уязвимость. Все вычисления (HMAC, SHA1) выполняются на клиенте. MitM внутри процесса полностью контролирует ответы.

### 16.2. Рекомендации для defensive research

**Для приватных серверов:**

1. **Не полагаться только на Warden** — behavioral analysis критичен (packet timing, inhuman reactions)

2. **Использовать custom check definitions** — не публиковать database openly

3. **Randomize check intervals** — предсказуемость позволяет временное отключение hooks

4. **Combine with server-side validation** — movement anti-cheat, spell cast validation, economy tracking

**Для исследователей безопасности:**

1. **Shadow Copy обязателен** — единственный reliable метод для MEM_CHECK/PROC_CHECK spoofing

2. **Variant A (modify plaintext)** — optimal для всех packet modifications

3. **Deferred RC4 hooking** — после HASH_REQUEST, иначе integrity hash fails

4. **Checksum recalculation** — любая модификация results section требует SHA1 XOR-fold recomputation

### 16.3. Ограничения системы

**Что Warden НЕ детектирует:**

- **Kernel-mode code** (если драйвер не в DRIVER_CHECK списке)
- **Hardware-based cheats** (DMA attacks через PCIe devices)
- **Network manipulation** (packet injection вне процесса)
- **Behavioral patterns** (inhuman reaction time, perfect rotations)

**Почему клиент-side anti-cheat уязвим:**

```
Атакующий контролирует среду выполнения
    ↓
Может hook любую функцию (включая Warden)
    ↓
Может модифицировать ответы до шифрования
    ↓
Сервер получает "чистые" данные
    ↓
Обход complete
```

**Единственное решение:** Server-side validation + behavioral analysis.

### 16.4. Будущие направления

**Post-3.3.5a эволюция Warden:**

- **Cataclysm/MoP:** Code virtualization, obfuscated dispatch
- **WoD+:** Kernel-mode components (driver-based)
- **Modern WoW:** Full kernel anti-cheat (Ring 0)

**Lessons learned:**

Client-side integrity checks fundamentally vulnerable. Modern games shift to:
- Server-authoritative game state
- Kernel-mode anti-cheat (EasyAntiCheat, BattlEye)
- Machine learning behavioral detection
- Hardware attestation (TPM-based)

---

## Приложения

### Приложение A. Каталог исследованных модулей Warden

| Module Hash | Decompressed Size | Runtime Size | Check Types Found | Dispatch Method | Notes |
|-------------|-------------------|--------------|-------------------|-----------------|-------|
| **7C4ABC97** | 29234 | ? | 9 types (0x1F, 0x22, 0x47, 0x69, 0x8E, 0x91, 0xB3, 0xD8, 0xDB) | Dispatch chain | Canonical module, used by emulators |
| **DA3BF29E** | ? | ? | TIMING=0x74, LUA=0x70 | ? | Different Type IDs confirmed |
| **9A95D199** | 28876 | ? | Dispatcher via XOR scan | ? | No CMP chain found |
| **CB9E43D6** | 31718 | 49152 | 10 types via remap cross-ref | Remap-only | Full hash: CB9E43D692620E7B698C5CE085163E6E |
| **0BE6B21C** | 30132 | 45056 | 4 types (0xB5, 0xCE, 0xE7, 0xE8) | Dispatch chain | RC4 uses EAX as context register |
| **E191991E** | ? | ? | 10 types, XOR@0x2b01 | Dispatch chain | 0x39=TIMING, 0xC5=LUA, 0xE8=MPQ, 0xDD=MEM, 0x2E/0x51/0x7F=PAGE (3 variants) |
| **2E9FE85D** | ? | 45056 | 10 types, XOR@0x5865 | Dispatch chain | RC4: EAX-variant. 0x08/0xE7=PAGE, 0x16=DRIVER, 0x41/0xBC=MPQ/LUA, 0x54=MEM, 0x70=TIMING |
| **473AAAA1** | ? | ? | ? | ? | Seen only as cached, not captured yet |
| **90278079** | ? | ? | 9 types from remap | Remap-only | Fixed xorByte=0x2F, all packets. Missing 0x2F (PROC?) и 0xB3 |
| **DE240190** | 30404 | 45056 | 10 types, XOR@0x3f82 | Dispatch chain | 0x65=TIMING, 0x81=LUA, 0xB9=MEM, 0xF3=DRIVER, 0x56/0x9D=PAGE, 0xC8=MPQ, 0x2B=MODULE |
| **3E02C87E** | ? | ? | BST dispatch | Binary Search Tree | "Phantom ID" problem module |

### Приложение B. Emulator canonical enum (hardcoded)

**TrinityCore/AzerothCore/MaNGOS — WardenCheckType:**

```cpp
enum WardenCheckType {
    MEM_CHECK       = 0xF3,  // 243
    PAGE_CHECK_A    = 0xB2,  // 178
    PAGE_CHECK_B    = 0xBF,  // 191
    MPQ_CHECK       = 0x98,  // 152
    LUA_STR_CHECK   = 0x8B,  // 139 (or LUA_EVAL_CHECK)
    DRIVER_CHECK    = 0x71,  // 113
    TIMING_CHECK    = 0x57,  // 87
    PROC_CHECK      = 0x7E,  // 126
    MODULE_CHECK    = 0xD9,  // 217
};
```

**РАБОТАЕТ ТОЛЬКО С МОДУЛЕМ:** `79C0768D657977D697E10BAD956CCED1`

### Приложение C. Python analysis scripts (examples)

#### XOR-anchored scanner

```python
import re

# Pattern: xor r8, [reg+4]
# Opcodes: 32 [40-7F] 04
pattern = re.compile(b'\x32[\x40-\x7F]\x04', re.DOTALL)

with open('module_decrypted.bin', 'rb') as f:
    data = f.read()

matches = []
for m in pattern.finditer(data):
    offset = m.start()
    matches.append(offset)
    print(f"Found XOR @ 0x{offset:04X}")

# For each match, disassemble next 200 bytes
# Look for CMP/JE chains
```

#### Type ID extractor

```python
import struct
from capstone import *

md = Cs(CS_ARCH_X86, CS_MODE_32)

def extract_cmp_values(code_bytes, start_offset):
    type_ids = []
    for insn in md.disasm(code_bytes, start_offset):
        if insn.mnemonic == 'cmp' and insn.op_str.startswith('al,'):
            imm = insn.operands[1].imm
            type_ids.append(imm)
    return type_ids

# Usage
with open('module.bin', 'rb') as f:
    f.seek(xor_match_offset + 3)  # After XOR instruction
    code = f.read(200)
    types = extract_cmp_values(code, xor_match_offset + 3)
    print(f"Type IDs: {[hex(t) for t in types]}")
```

### Приложение D. Works Cited (Combined)

1. vmangos/warden_modules: Modules needed to use the warden anticheat. - GitHub, https://github.com/vmangos/warden_modules
2. warden_checks | TrinityCore MMo Project Wiki, https://trinitycore.info/database/335/world/warden_checks
3. Warden.h File Reference - AzerothCore Doxygen, https://www.azerothcore.org/doxygen/d7/dc7/Warden_8h.html
4. k-kowalski/SpellFire: World of Warcraft hacking framework - GitHub, https://github.com/k-kowalski/SpellFire
5. Warden MODULE_CHECK not working (may be user error?) - getMaNGOS, https://www.getmangos.eu/forums/topic/10797-warden-module_check-not-working-may-be-user-error/
6. warden module interprets the check incorrectly · Issue #28138 - GitHub, https://github.com/TrinityCore/TrinityCore/issues/28138
7. Warden - I now have MaNGOS managing it - getMaNGOS, https://www.getmangos.eu/forums/topic/5658-warden-i-now-have-mangos-managing-it/
8. TrinityCore: Warden Class Reference - Huihoo, https://docs.huihoo.com/doxygen/trinitycore/db/d5c/classWarden.html
9. Wiki: Warden Modules | SkullSecurity Blog, https://www.skullsecurity.org/wiki/Warden_Modules
10. AzerothCore WardenWin Implementation | PDF | Gnu | Software Development - Scribd, https://www.scribd.com/document/582975372/WardenWin
11. RC4 Encryption Algorithm Stream Ciphers Defined - Okta, https://www.okta.com/identity-101/rc4-stream-cipher/
12. WoWTools/src/WoWPacketViewer/Parsers/Warden/SMSG_WARDEN_DATA.cs - GitHub, https://github.com/tomrus88/WoWTools/blob/master/src/WoWPacketViewer/Parsers/Warden/SMSG_WARDEN_DATA.cs
13. namreeb/WardenSigning: RSA verification code + 72 binary modules, https://github.com/namreeb (archived from Neo2003)
14. xakepru/x14.08-coverstory-blizzard: Client-side RE code (C/C++), https://github.com/xakepru/x14.08-coverstory-blizzard

---

**Конец компендиума.**

*Версия 1.0 — Февраль 2026*

*Total pages: ~150 equivalent A4 pages*

*Combined research from 13 independent sources*
