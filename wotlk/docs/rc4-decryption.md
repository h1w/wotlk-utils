# RC4 Расшифровка CMSG_WARDEN_DATA

Этот документ объясняет, как мы расшифровываем исходящие ответы клиента на запросы Warden.

## 1. Зачем нужна расшифровка CMSG

### Что мы уже можем видеть
- **SMSG_WARDEN_DATA** (сервер → клиент): мы перехватываем через return-address hijack **ПОСЛЕ** того, как Warden handler расшифровал пакет
- Мы видим расшифрованные данные: MODULE_USE, HASH_REQUEST, CHEAT_CHECKS_REQUEST и т.д.
- Это работает отлично

### Проблема с CMSG (РЕШЕНА)
- **CMSG_WARDEN_DATA** (клиент → сервер): ответы клиента на запросы Warden
- Мы перехватываем эти пакеты в хуке SendPacket (адрес 0x00632B50)
- К этому моменту данные **уже зашифрованы** RC4 cipher'ом Warden модуля
- **РЕШЕНИЕ**: внутренний хук на RC4 PRGA функцию внутри Warden модуля — захватываем plaintext ДО шифрования

### Почему это важно
Чтобы понять, что происходит в диалоге Warden ↔ клиент, нам нужно видеть **обе стороны**:
- Что сервер **спрашивает** (SMSG) — это мы видим ✓
- Что клиент **отвечает** (CMSG) — **теперь мы видим тоже** ✓

С расшифровкой CMSG мы теперь знаем:
- Какие хеши клиент отправил на HASH_REQUEST
- Какие результаты MEM_CHECK клиент вернул
- Что клиент ответил на LUA_EVAL проверку
- Полную картину диалога Warden ↔ клиент

## 2. Два разных RC4 cipher'а

В WoW 3.3.5a используются **два отдельных RC4 шифра**. Начинающие часто их путают.

### Session cipher (ARC4::Process @ 0x00774EA0)
**Назначение**: шифрует заголовки ВСЕХ сетевых пакетов (не только Warden!)

**Что шифрует**:
- CMSG header: 2 байта (размер пакета) + 4 байта (opcode) = 6 байт
- SMSG header: 4 байта

**Что НЕ шифрует**:
- Payload (содержимое) пакетов

**Расположение**: код в исполняемом файле Wow.exe (адрес 0x00774EA0)

**Ключ**: генерируется при логине из сессионного ключа (session key)

**Это НЕ то, что нам нужно для Warden payload!**

### Warden cipher
**Назначение**: шифрует payload ТОЛЬКО Warden пакетов (0x2E6 SMSG, 0x2E7 CMSG)

**Что шифрует**:
- Весь payload после opcode (MODULE_USE, HASH_REQUEST, CHEAT_CHECKS_REQUEST и т.д.)

**Расположение**: внутри blob'а Warden модуля (загружается в VirtualAlloc, MEM_PRIVATE память)

**S-box'ы**: **ДВА отдельных** S-box'а в памяти модуля:
- Один для **decrypt** (расшифровка входящих SMSG от сервера)
- Один для **encrypt** (зашифровка исходящих CMSG к серверу)

**Формат S-box**: uint8_t S[256] — массив из 256 байт (подтверждено сканированием)

**Ключ**: отправляется сервером в пакете MODULE_INITIALIZE (16 байт RC4 ключа)

**Это то, что нам нужно!**

## 3. Как работает RC4

RC4 (Rivest Cipher 4) — потоковый шифр. Очень простой алгоритм, объясню по шагам.

### Key Scheduling Algorithm (KSA) — инициализация
На входе: ключ произвольной длины (в Warden — 16 байт)

```cpp
// Инициализируем S-box значениями 0..255
for (int i = 0; i < 256; i++) {
    S[i] = i;
}

// Перемешиваем S-box на основе ключа
int j = 0;
for (int i = 0; i < 256; i++) {
    j = (j + S[i] + key[i % key_length]) % 256;
    swap(S[i], S[j]);
}
```

После KSA:
- **S-box** содержит перестановку чисел 0-255 (все уникальные)
- Два счётчика **i** и **j** обнуляются

### Pseudo-Random Generation Algorithm (PRGA) — шифрование/расшифровка
Для каждого байта данных:

```cpp
i = (i + 1) % 256;           // Увеличиваем i
j = (j + S[i]) % 256;        // Обновляем j на основе S[i]
swap(S[i], S[j]);            // Перемешиваем S-box

uint8_t K = S[(S[i] + S[j]) % 256];  // Генерируем keystream байт
output = input ^ K;          // XOR с данными
```

**Ключевое свойство RC4**: encrypt и decrypt — это **одна и та же операция**!
- plaintext XOR keystream = ciphertext
- ciphertext XOR keystream = plaintext

Потому что XOR обратим: `(A ^ B) ^ B = A`

### Почему два S-box'а в Warden
Warden общается по схеме request-response:
- Сервер → клиент (SMSG): клиент **decrypt** с помощью decrypt S-box
- Клиент → сервер (CMSG): клиент **encrypt** с помощью encrypt S-box

Оба S-box'а инициализируются **одним и тем же ключом** из MODULE_INITIALIZE, но работают **независимо** (разные i, j, состояние S).

## 4. Решение: внутренний хук RC4 (PRIMARY — warden_rc4_hook.cpp)

### 4.1 Идея
Вместо клонирования S-box'ов из памяти (подход с race condition), мы **хукаем RC4 PRGA функцию** внутри Warden модуля.

**Преимущества**:
- Захватываем plaintext **ДО** шифрования (на входе функции)
- Нет зависимости от timing (не важно, когда клонировать S-box)
- Работает на всех известных модулях (подтверждено на 10/10)

**Как это работает**:
1. Находим RC4 PRGA функцию в памяти модуля (pattern scanner)
2. Устанавливаем MinHook на эту функцию
3. Наш detour перехватывает вызов ПЕРЕД оригинальной функцией
4. Проверяем: это CMSG encryption (третий вызов после SMSG decryption)?
5. Если да — сохраняем plaintext в thread-safe буфер
6. SendPacketHandler забирает plaintext через ConsumePlaintext()

### 4.2 Pattern Scanner (ScanRuntimeForAllRC4)

**Цель**: найти **ВСЕ** RC4 PRGA функции в runtime памяти Warden модуля

**Проблема**: нет стабильной byte signature (разные модули — разные регистры, разный код). Более того, некоторые модули используют **разные RC4 функции** для main thread и module thread.

**Решение**: сканируем инструкции, которые **обращаются к полям RC4 context**, и хукаем **все** найденные функции (до 4).

**Context layout**:
```
[S[256]]  // 256 байт — S-box массив
[i]       // 1 байт  — индекс i (смещение +0x100)
[j]       // 1 байт  — индекс j (смещение +0x101)
```

**Что ищем**:
- Инструкции с displacement = 0x100 (обращение к i)
- Инструкции с displacement = 0x101 (обращение к j)
- Типы инструкций: MOVZX (0F B6), MOV byte (88), ADD byte (00), INC/DEC byte (FE), LEA (8D)

**Алгоритм**:
1. Сканируем runtime память модуля (найденную в MODULE_INITIALIZE)
2. Для каждого адреса проверяем: есть ли инструкция с disp32=0x100 или 0x101?
3. Группируем совпадения в кластеры (gap <= 120 байт между соседними совпадениями)
4. Для каждого кластера проверяем:
   - >= 2 совпадения в кластере
   - Есть ссылки на ОБОИХ (0x100 и 0x101)
5. Для каждого подходящего кластера вызываем `FindFunctionPrologue`:
   - Ищем назад `push ebp; mov ebp, esp` (55 8B EC) — стандартный frame
   - Или INT3 (0xCC) / RET (0xC2/0xC3) boundary
6. Дедупликация: разные кластеры могут резолвиться в одну функцию
7. Результат: вектор уникальных адресов (до `kMaxRC4Hooks=4`)

**Результат**:
```
[RC4_HOOK] Found 15 instructions referencing 0x100/0x101 in module memory
[RC4_HOOK] 3 cluster(s) found in module
[RC4_HOOK] Cluster #1: 6 matches at module+0x10ed-0x111d [has i] [has j]
[RC4_HOOK]   -> function at 0x1bea10e0 (module+0x10e0) prologue=[55 8B EC ...]
[RC4_HOOK] Cluster #2: 4 matches at module+0x13c7-0x13d7 [has i] [has j]
[RC4_HOOK]   -> function at 0x1bea13b0 (module+0x13b0) prologue=[55 8B EC ...]
[RC4_HOOK] 2 hook(s) installed successfully (out of 2 candidate(s))
```

**Тестирование**:
- 10/10 модулей offline (Python анализ дампов): все нашли PRGA
- Runtime (игра): все протестированные модули успешно хукаются
- Модули с несколькими RC4 функциями (0BE6B21C): все функции хукаются

### 4.3 Calling Convention

**Поддерживается 6 конвенций** (auto-detection при первом вызове):

| # | Convention | Context | Data | Length |
|---|-----------|---------|------|--------|
| 1 | __thiscall | ECX | stk1 | stk2 |
| 3 | variant | ECX | stk2 | stk1 |
| 4 | EDX-variant | EDX | stk1 | stk2 |
| 5 | EAX-variant | EAX | stk1 | stk2 |
| 6 | EAX-variant2 | EAX | stk2 | stk1 |
| 2 | __cdecl | stk1 | stk2 | stk3 |

**Порядок проверки**: регистровые конвенции первыми (более специфичные), stack-based __cdecl последним (наименее специфичный — стековый аргумент может случайно совпасть с S-box).

**Примеры модулей**:
- Большинство модулей: ECX=ctx (__thiscall)
- Модуль **0BE6B21C**: EAX=ctx (convention 5)

**Context layout** (подтверждён):
```cpp
struct RC4_Context {
    uint8_t S[256];  // +0x000..0x0FF
    uint8_t i;       // +0x100
    uint8_t j;       // +0x101
};
```

**Prologue**:
- Стандартный frame: `push ebp; mov ebp, esp` (55 8B EC)
- Выбор регистров varies (EDI/ESI/EBX/EAX), но frame всегда одинаковый

**Auto-detection** (через `LooksLikeRC4Context` + permutation check):
1. Проверяем каждый регистр: ECX, EDX, EAX (от специфичных к общим)
2. Для каждого: `LooksLikeRC4Context(reg)` — readable memory? S[256] is permutation?
3. Если найден — определяем порядок data/len по `IsValidPointer` + `IsReasonableLength`
4. Если ни один регистр не подошёл — проверяем stk1 как ctx (__cdecl)
5. Convention shared across all hook slots (определяется один раз)

**Диагностика**: первые 8 вызовов логируют ВСЕ регистры (EAX, ECX, EDX, EBX, ESI, EDI) + stk1-3 для отладки

**Результат**: 100% автоматическое определение без hardcode

### 4.4 Hook Implementation (Multi-Hook)

**Архитектура**: до 4 одновременных хуков, каждый со своим naked stub и trampoline.

**4 отдельных naked stub'а** (необходимы, потому что каждый прыгает в свой trampoline):
```cpp
__declspec(naked) static void HookedRC4Naked_0() {
    __asm {
        pushad
        pushfd
        mov eax, esp
        push eax
        call RC4DetourHandler    // Общий handler для всех слотов
        add esp, 4
        popfd
        popad
        jmp dword ptr [g_trampolines + 0]   // Trampoline слота 0
    }
}

// HookedRC4Naked_1 → g_trampolines + 4
// HookedRC4Naked_2 → g_trampolines + 8
// HookedRC4Naked_3 → g_trampolines + 12
```

**Общий detour handler** (`RC4DetourHandler`):
- Получает ESP, извлекает ВСЕ регистры из pushad/pushfd стека
- Convention detection и plaintext capture — shared для всех слотов
- Thread-safe call counting через `InterlockedIncrement`

**Массивы состояния**:
```cpp
static constexpr int kMaxRC4Hooks = 4;
static void*     g_trampolines[kMaxRC4Hooks];  // Trampoline для каждого хука
static uintptr_t g_hookedAddrs[kMaxRC4Hooks];  // Адрес хука
static bool      g_hooksActive[kMaxRC4Hooks];  // Активен ли хук
static int       g_numHooks;                    // Сколько хуков установлено
```

**MinHook** (для каждой найденной функции):
```cpp
for (size_t i = 0; i < funcAddrs.size() && installed < kMaxRC4Hooks; ++i) {
    MH_CreateHook(funcAddrs[i], g_nakedStubs[installed], &g_trampolines[installed]);
    MH_EnableHook(funcAddrs[i]);
    g_hookedAddrs[installed] = funcAddrs[i];
    g_hooksActive[installed] = true;
    ++installed;
}
```

**Thread safety**: CRITICAL_SECTION защищает:
- `g_capturedPlaintext` (буфер с plaintext)
- `g_capturedPlaintextLen` (длина plaintext)
- `g_hasCapturedPlaintext` (флаг готовности)

### 4.5 Plaintext Capture Flow

**Последовательность вызовов RC4 при обработке SMSG_WARDEN_DATA**:

1. **Call #1**: Decrypt SMSG payload (часть 1)
   - ECX = invalid/small value (0x39, 0x3d...) — LooksLikeRC4Context(ECX) = false
   - Пропускаем (convention detection rejects)

2. **Call #2**: Decrypt SMSG payload (часть 2)
   - ECX = invalid/small value
   - Пропускаем

3. **Call #3**: **Encrypt CMSG payload** (это нужно захватить!)
   - ECX = valid RC4_Context* (указывает на readable memory)
   - S[256] is permutation → LooksLikeRC4Context(ECX) = true
   - data buffer ещё содержит **plaintext** (шифрование не началось)
   - Валидируем структуру CMSG (ValidateDecryptedCmsg)
   - Если валидация успешна → копируем в `g_capturedPlaintext`

**Validation** (ValidateDecryptedCmsg):
```cpp
bool ValidateDecryptedCmsg(const uint8_t* data, uint32_t len) {
    if (len == 0) return false;

    uint8_t opcode = data[0];
    switch (opcode) {
        case 0x00:  // MODULE_MISSING
        case 0x01:  // MODULE_OK
        case 0x05:  // MODULE_FAILED
            return len == 1;

        case 0x04:  // HASH_RESULT
            return len == 21;  // 1 opcode + 20 SHA1

        case 0x02:  // CHEAT_CHECKS_RESULT
            if (len < 7) return false;  // 1 opcode + 2 length + 4 checksum
            uint16_t resultLen = *(uint16_t*)(data + 1);
            return len == 7 + resultLen;

        default:
            return false;  // Unknown opcode
    }
}
```

**Capture**:
```cpp
EnterCriticalSection(&g_rc4CS);
if (ValidateDecryptedCmsg(data, len)) {
    memcpy(g_capturedPlaintext, data, len);
    g_capturedPlaintextLen = len;
    g_hasCapturedPlaintext = true;
}
LeaveCriticalSection(&g_rc4CS);
```

### 4.6 Distinguishing SMSG from CMSG calls

**Проблема**: RC4 PRGA вызывается для ОБОИХ направлений (SMSG decrypt, CMSG encrypt)

**Решение**: auto-detection по ECX
- **SMSG decrypt calls**: ECX = small/invalid values (0x39, 0x3d...) — LooksLikeRC4Context(ECX) = false
- **CMSG encrypt calls**: ECX = valid RC4_Context* — LooksLikeRC4Context(ECX) = true

**LooksLikeRC4Context**:
```cpp
bool LooksLikeRC4Context(void* ptr) {
    if (!ptr) return false;

    // Проверяем: readable memory?
    if (IsBadReadPtr(ptr, 258)) return false;

    // Читаем S[256]
    uint8_t* S = (uint8_t*)ptr;

    // Проверяем: это перестановка 0-255?
    bool seen[256] = {false};
    for (int i = 0; i < 256; i++) {
        if (seen[S[i]]) return false;  // Дубликат
        seen[S[i]] = true;
    }
    return true;  // Все 0-255 встречаются ровно 1 раз
}
```

**Результат**:
- SMSG calls: пропускаются (ECX не проходит LooksLikeRC4Context)
- CMSG calls: захватываются (ECX проходит валидацию + CMSG structure validation)

### 4.7 Installation Lifecycle

**Installation** (в MODULE_INITIALIZE handler):
```cpp
// 1. Находим модуль в памяти (FindModuleInMemory)
// 2. Сканируем ВСЕ RC4 PRGA функции (ScanRuntimeForAllRC4)
std::vector<uintptr_t> funcAddrs = ScanRuntimeForAllRC4(moduleAddr, moduleSize);
if (funcAddrs.empty()) {
    LOG(ERROR) << "No RC4 functions found, falling back to S-box cloning";
    return;
}

// 3. Устанавливаем хуки (до kMaxRC4Hooks=4)
for (size_t i = 0; i < funcAddrs.size() && installed < kMaxRC4Hooks; ++i) {
    MH_CreateHook(funcAddrs[i], g_nakedStubs[installed], &g_trampolines[installed]);
    MH_EnableHook(funcAddrs[i]);
    // ... track in g_hookedAddrs[], g_hooksActive[]
}
g_numHooks = installed;

LOG(INFO) << installed << " hook(s) installed successfully";
```

**Removal** (в MODULE_USE handler + Shutdown):
```cpp
// MODULE_USE: сервер загружает новый модуль → старые хуки станут invalid
for (int i = 0; i < kMaxRC4Hooks; ++i) {
    if (!g_hooksActive[i]) continue;
    MH_DisableHook(g_hookedAddrs[i]);
    MH_RemoveHook(g_hookedAddrs[i]);
    g_hooksActive[i] = false;
    g_hookedAddrs[i] = 0;
    g_trampolines[i] = nullptr;
}
g_numHooks = 0;
```

**IsActive**: возвращает `g_numHooks > 0`

**Thread safety**: CRITICAL_SECTION инициализируется через `EnsureLock()` (lazy init)

## 5. Fallback: клонирование S-box (warden_rc4.cpp)

Если pattern scanner не может найти RC4 PRGA (гипотетический сценарий — пока 10/10 модулей работают), используется **fallback** — клонирование S-box'ов из памяти.

**Проблема**: race condition между клонированием и шифрованием

### 5.1 Алгоритм

**Шаг 1: Сканирование** (ScanForRC4States)
- Перебираем все MEM_PRIVATE memory regions
- Для каждого адреса проверяем: это S-box (перестановка 0-255)?
- Проверяем два layout'а:
  - Layout A: `[i][j][S[256]]` — индексы перед массивом
  - Layout B: `[S[256]][i][j]` — индексы после массива
- Результат: ~38 кандидатов (19 адресов × 2 layout'а)

**Шаг 2: Клонирование** (CloneAllStates в WardenPreHandler)
- Копируем состояние всех кандидатов (i, j, S[256])
- Thread-safe (CRITICAL_SECTION)
- Timing: ПЕРЕД Warden handler (но это не помогает — модуль работает в другом потоке)

**Шаг 3: Попытка расшифровки** (DecryptCmsg в SendPacket hook)
- Перебираем все клоны
- Для каждого: симулируем RC4_Process, расшифровываем payload
- Structural validation: проверяем формат CMSG (opcode + length)
- Lock in: если ровно 1 кандидат прошёл → запоминаем индекс

### 5.2 Почему НЕ работает

**Race condition**:
1. WardenPreHandler клонирует S-box'ы (main thread)
2. Warden handler обрабатывает SMSG (module thread)
3. Warden handler формирует CMSG (module thread)
4. **Warden handler шифрует CMSG** (module thread) — S-box изменяется!
5. SendPacket hook пытается расшифровать (main thread) — но S-box уже другой

**Результат**: "No candidate passed structural validation" — ни один клон не подходит

### 5.3 Когда используется

**Только как fallback** если internal hook fails:
```cpp
// SendPacketHandler
bool decrypted = false;
uint8_t plaintext[512];
uint32_t plainLen = 0;

// PRIMARY: internal RC4 hook
if (warden_rc4_hook::IsActive()) {
    decrypted = warden_rc4_hook::ConsumePlaintext(plaintext, sizeof(plaintext), &plainLen);
}

// FALLBACK: S-box cloning
if (!decrypted && copyLen > 0) {
    decrypted = warden_rc4::DecryptCmsg(localBuf, copyLen, plaintext, sizeof(plaintext));
}

if (decrypted) {
    LOG(INFO) << "CMSG_WARDEN decrypted, len=" << plainLen
              << " [" << (warden_rc4_hook::IsActive() ? "rc4_hook" : "fallback") << "]";
}
```

## 6. Реальные результаты из лога

### Установка хуков (multi-hook)
```
[RC4_HOOK] Found 15 instructions referencing 0x100/0x101 in module memory
[RC4_HOOK] 3 cluster(s) found in module
[RC4_HOOK] Cluster #1: 3 matches at module+0x0fc3-0x0fd3 [has i] [NO j]
[RC4_HOOK]   -> skipped (missing i or j reference)
[RC4_HOOK] Cluster #2: 6 matches at module+0x10ed-0x111d [has i] [has j]
[RC4_HOOK]   -> function at 0x1bea10e0 (module+0x10e0) prologue=[55 8B EC ...]
[RC4_HOOK] Cluster #3: 4 matches at module+0x13c7-0x13d7 [has i] [has j]
[RC4_HOOK]   -> function at 0x1bea13b0 (module+0x13b0) prologue=[55 8B EC ...]
[RC4_HOOK] Hook #1 installed at 0x1bea10e0
[RC4_HOOK] Hook #2 installed at 0x1bea13b0
[RC4_HOOK] 2 hook(s) installed successfully (out of 2 candidate(s))
```

**Cluster #1** (3 matches, only i) = KSA (Key Scheduling Algorithm) — пропущен
**Cluster #2** (6 matches, i+j) = RC4 PRGA основная функция — хук #1
**Cluster #3** (4 matches, i+j) = вторая RC4 функция (module thread) — хук #2

### Захват CMSG
```
[RC4_HOOK] RC4 called: ECX=0x00b2d828 data=0x0d90e4d8 len=12
[RC4_HOOK] LooksLikeRC4Context(ECX) = true
[RC4_HOOK] Captured CMSG plaintext: len=12 warden_op=0x02 (CHEAT_CHECKS_RESULT)
[CMSG_WARDEN] decrypted=[02 05 00 90 0D D5 28 01 F8 9E BE 00] warden_op=0x02 [rc4_hook]
```

**Разбор CMSG payload**:
```
02             — opcode (CHEAT_CHECKS_RESULT)
05 00          — length (5 байт результатов)
90 0D D5 28    — checksum (4 байта)
01 F8 9E BE 00 — result data (5 байт — per-check results)
```

### Статистика сессии
```
[WARDEN] Session summary:
  SMSG_WARDEN_DATA packets: 8
  CMSG_WARDEN_DATA packets: 6
  CMSG decrypted: 6/6 (100%)
  Decryption method: [rc4_hook] (primary)
  Fallback used: 0 times
```

**100% success rate** — все CMSG пакеты расшифрованы через internal hook

## 7. Two-tier decryption в SendPacketHandler

**Стратегия**: primary (internal hook) + fallback (S-box cloning)

```cpp
void SendPacketHandler(CDataStore* ds) {
    // 1. Копируем CDataStore в локальный буфер (thread safety)
    uint8_t localBuf[512];
    uint32_t copyLen = min(ds->size, sizeof(localBuf));
    memcpy(localBuf, ds->buffer, copyLen);

    // 2. Проверяем opcode (первые 4 байта = opcode)
    if (copyLen < 4) return;
    uint32_t opcode = *(uint32_t*)localBuf;
    if (opcode != 0x2E7) return;  // Not CMSG_WARDEN_DATA

    // 3. PRIMARY: internal RC4 hook
    bool decrypted = false;
    uint8_t plaintext[512];
    uint32_t plainLen = 0;

    if (warden_rc4_hook::IsActive()) {
        decrypted = warden_rc4_hook::ConsumePlaintext(plaintext, sizeof(plaintext), &plainLen);
    }

    // 4. FALLBACK: S-box cloning
    if (!decrypted && copyLen > 4) {
        uint8_t* payload = localBuf + 4;
        uint32_t payloadLen = copyLen - 4;
        decrypted = warden_rc4::DecryptCmsg(payload, payloadLen, plaintext, sizeof(plaintext));
        plainLen = payloadLen;
    }

    // 5. Логируем результат
    if (decrypted) {
        const char* method = warden_rc4_hook::IsActive() ? "rc4_hook" : "fallback";
        uint8_t wardenOp = (plainLen > 0) ? plaintext[0] : 0xFF;

        LOG(INFO) << "CMSG_WARDEN decrypted: len=" << plainLen
                  << " warden_op=0x" << std::hex << (int)wardenOp
                  << " [" << method << "]";

        // Hex dump
        std::stringstream ss;
        for (uint32_t i = 0; i < min(plainLen, 64u); i++) {
            ss << std::hex << std::setw(2) << std::setfill('0') << (int)plaintext[i] << " ";
        }
        LOG(INFO) << "  payload: " << ss.str();
    } else {
        LOG(WARNING) << "CMSG_WARDEN decryption failed";
    }
}
```

**ConsumePlaintext** (one-shot, thread-safe):
```cpp
bool ConsumePlaintext(uint8_t* out, uint32_t outSize, uint32_t* outLen) {
    EnterCriticalSection(&g_rc4CS);

    bool result = false;
    if (g_hasCapturedPlaintext) {
        uint32_t copyLen = min(g_capturedPlaintextLen, outSize);
        memcpy(out, g_capturedPlaintext, copyLen);
        *outLen = copyLen;

        // Clear after read (one-shot)
        g_hasCapturedPlaintext = false;
        result = true;
    }

    LeaveCriticalSection(&g_rc4CS);
    return result;
}
```

## 8. Текущий статус

### PRIMARY (warden_rc4_hook.cpp)
- **Статус**: ПОЛНОСТЬЮ РАБОТАЕТ
- **Success rate**: 100% (6/6 CMSG packets decrypted)
- **Pattern scanner**: работает на 10/10 модулях (offline Python analysis + runtime)
- **Multi-hook**: до 4 одновременных хуков (для модулей с несколькими RC4 функциями)
- **Calling convention**: auto-detected (6 вариантов: ECX/EDX/EAX/stack-based, обычный/swapped порядок)
- **Thread safety**: CRITICAL_SECTION + InterlockedIncrement (Warden module thread vs main thread)
- **Stability**: нет crashes, нет disconnects, Warden integrity checks pass

### FALLBACK (warden_rc4.cpp)
- **Статус**: установлен, но не используется
- **Использование**: 0/6 packets (primary работает, fallback не нужен)
- **Проблема**: race condition (S-box изменяется между clone и decrypt)
- **Цель**: safety net для гипотетических модулей (пока все 10/10 работают с primary)

### Pattern Scanner Validation
**Offline testing** (Python analysis на дампах):
- 10/10 modules: RC4 PRGA найден
- 8/10 modules: dispatch chain найден (XOR-anchored scan)
- 10/10 modules: remap table найден (но бесполезен для type extraction)

**Runtime testing**:
- Все протестированные модули (7C4ABC97, DA3BF29E, 9A95D199): pattern scanner успешно находит PRGA
- Hook устанавливается без ошибок
- Trampoline создаётся MinHook на PAGE_EXECUTE_READWRITE страницах

## 9. Что реализовано после RC4 расшифровки

### 9.1 Per-check result parsing (РЕШЕНО)
Все форматы результатов разобраны и подтверждены:
- TIMING: 5 bytes (flag:1 + ticks:4)
- MEM: 1 byte if fail / 1+readLen if OK
- PAGE/PROC/MODULE/DRIVER: 1 byte (0xE9=pass)
- MPQ: 1 byte if fail / 1+20 (SHA1) if OK
- LUA: 1 byte if fail / 1+1+strlen if OK

Реализовано в `ParseCheatChecksResult()` (hooks.cpp).

### 9.2 Checksum algorithm (РЕШЕНО)
**Алгоритм**: SHA1(results) -> 5 x uint32_t LE -> XOR-fold -> uint32_t
Реализовано в `warden_checksum.cpp` через WinCrypt CALG_SHA1.
Все наблюдаемые checksums валидируются корректно.

### 9.3 CMSG modification — MEM_CHECK / PAGE_CHECK spoofing (РЕШЕНО)
**Variant A** реализован в `warden_spoof.cpp`:
1. `SpoofCmsgIfNeeded()` вызывается из RC4 hook ДО шифрования
2. Peek front FIFO queue (pending checks с адресами)
3. Walk results — для MEM_CHECK на hook-адрес: заменяет данные на оригинальные из shadow_copy
4. Для PAGE_CHECK на hook-адрес: форсирует 0xE9 (pass)
5. Пересчитывает checksum
6. Записывает обратно в буфер модуля — оригинальный RC4 шифрует spoofed данные

**Variant D** (disable all hooks) был реализован и отброшен — Warden модуль работает асинхронно (17-28 сек задержка на отдельном потоке), отключение хуков на ~1мс в main thread бесполезно.

### 9.4 Multiple RC4 functions per module (РЕШЕНО)
`ScanRuntimeForAllRC4` находит ВСЕ RC4 функции, `Install` хукает до 4 одновременно.

### 9.5 Что осталось (TODO)
- **LUA_EVAL spoofing**: подмена LUA results в CMSG
- **Module RC4 offset caching**: кеширование offsets RC4 функций по hash модуля (optimization)

---

## Заключение

**RC4 расшифровка CMSG полностью решена** через internal hook на RC4 PRGA функцию внутри Warden модуля. **MEM_CHECK / PAGE_CHECK spoofing реализован** через Variant A (модификация plaintext перед RC4 шифрованием).

**Ключевые достижения**:
- 100% success rate расшифровки CMSG
- Module-agnostic pattern scanner (10/10 offline modules, все runtime-тесты)
- Multi-hook: до 4 одновременных хуков (модули с несколькими RC4 функциями)
- 6 поддерживаемых calling conventions (auto-detection)
- Checksum: SHA1 XOR-fold — решён, валидируется, пересчитывается
- **Spoofing (Variant A)**: MEM_CHECK/PAGE_CHECK результаты подменяются на оригинальные байты из shadow copy
- Request-response correlation через FIFO queue
- Thread-safe (CRITICAL_SECTION + InterlockedIncrement)
- Two-tier architecture (primary RC4 hook + S-box cloning fallback)
