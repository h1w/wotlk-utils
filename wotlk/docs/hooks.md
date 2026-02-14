# Хуки (Hooks) в wotlk-utils

## 1. Что такое хук (hook)

Хук — это способ перехватить вызов функции в чужом процессе и выполнить свой код до/вместо/после оригинального кода.

### Inline hook
Перезаписываем первые 5+ байт оригинальной функции на инструкцию `JMP` (переход) к нашему коду. Когда программа вызывает оригинальную функцию, управление передаётся нашему коду.

Пример (в памяти):
```
Оригинал:        55 8B EC 83 EC 10 ...  (начало функции)
После хука:      E9 XX XX XX XX 90 ...  (JMP к нашему коду + NOP)
```

### Trampoline
Библиотека MinHook сохраняет оригинальные байты функции в отдельный буфер (trampoline) и дописывает в конец `JMP` обратно к оригинальной функции. Это позволяет из нашего кода вызвать оригинальную функцию.

```
Наш код → вызывает trampoline → выполняет оригинальные инструкции → JMP в середину оригинальной функции
```

### __declspec(naked)
Когда calling convention функции нестандартная (не __cdecl, не __stdcall, не __thiscall), компилятор не может сгенерировать правильный пролог/эпилог. В таких случаях пишем функцию на ассемблере с атрибутом `__declspec(naked)` — компилятор не добавляет никакого кода, только то что мы написали.

Типичная структура naked функции:
```cpp
__declspec(naked) void MyHook() {
    __asm {
        pushad          // Сохраняем все регистры
        pushfd          // Сохраняем флаги
        // ... наш код ...
        popfd           // Восстанавливаем флаги
        popad           // Восстанавливаем регистры
        jmp [original]  // Прыгаем в оригинальную функцию
    }
}
```

---

## 2. Все 4 хука в проекте

### 2.1. FrameScript_Execute (0x00819210)

**Зачем:** Логирование всего Lua кода, который выполняет клиент WoW.

**Calling convention:** `__cdecl` (стандартная для C функций)

**Сигнатура:**
```cpp
void __cdecl FrameScript_Execute(const char* code, const char* filename, int unused);
```

**Параметры:**
- `code` — строка с Lua кодом для выполнения
- `filename` — имя файла или источника кода (может быть `@Interface\...` для стандартных UI скриптов)
- `unused` — не используется

**Что делаем:**
1. Проверяем `filename`: если начинается с `@Interface` — это стандартный UI скрипт, пропускаем (чтобы не засорять лог)
2. Если это пользовательский код — логируем `filename` и `code`
3. Вызываем оригинальную функцию через trampoline

**Пример лога:**
```
[LUA] Executing: @custom_addon.lua
      Code: print("Hello from addon")
```

**Полезность:** Можно отследить какие аддоны и макросы выполняются, найти вредоносный код.

---

### 2.2. SMSG_WARDEN_DATA Handler (0x007DA850)

**Зачем:** Это центральная точка анализа системы античита Warden. Здесь обрабатываются все входящие пакеты от сервера с запросами проверок.

**Calling convention:** **НЕ стандартная!** Это не `__thiscall` и не `__cdecl`.

**Stack layout при вызове функции:**
```
[ESP+0x00] = return address (адрес возврата)
[ESP+0x04] = 0 (неизвестный параметр, всегда 0)
[ESP+0x08] = opcode (0x2E6 = SMSG_WARDEN_DATA)
[ESP+0x0C] = ECX (дубликат регистра ECX)
[ESP+0x10] = CDataStore* (указатель на структуру с данными пакета)
```

**CDataStore структура:**
```
+0x00: vtable pointer (4 байта)
+0x04: buffer (uint8_t*) — указатель на данные пакета
+0x10: size (uint32_t) — размер данных
+0x14: readPos (uint32_t) — текущая позиция чтения (начинается с 2, т.к. opcode уже прочитан)
```

**КРИТИЧЕСКАЯ ПРОБЛЕМА:**
В момент вызова этой функции данные пакета **зашифрованы RC4**. Расшифровка происходит **ВНУТРИ** оригинального handler'а. Если мы просто прочитаем `buffer`, мы получим мусор.

**РЕШЕНИЕ: Return-Address Hijack**

Мы используем технику подмены адреса возврата:

1. **WardenPreHandler** (наш код) вызывается **ДО** оригинального handler'а
2. Сохраняем `CDataStore*` и `readPos` в глобальные переменные
3. Находим на стеке return address и **заменяем** его на адрес нашего **WardenPostHandler**
4. Возвращаем управление — оригинальный handler выполняется и **расшифровывает данные**
5. Оригинальный handler делает `RET` — но попадает не к настоящему caller'у, а в наш **WardenPostHandler**
6. **WardenPostHandler** читает уже **расшифрованные** данные из `CDataStore`
7. WardenPostHandler восстанавливает оригинальный return address и прыгает туда (`jmp eax`)

**Код (упрощённо):**
```cpp
// Глобальные переменные
static CDataStore* g_lastDataStore = nullptr;
static uint32_t g_lastReadPos = 0;
static void* g_originalReturnAddress = nullptr;

__declspec(naked) void WardenPreHandler() {
    __asm {
        pushad
        pushfd
    }

    // Сохраняем CDataStore и readPos
    __asm {
        mov eax, [esp + 0x30]  // CDataStore* (после pushad/pushfd)
        mov g_lastDataStore, eax
        mov eax, [eax + 0x14]  // readPos
        mov g_lastReadPos, eax

        // Подменяем return address
        mov eax, [esp + 0x24]  // оригинальный ret addr
        mov g_originalReturnAddress, eax
        lea eax, WardenPostHandler
        mov [esp + 0x24], eax  // новый ret addr = PostHandler
    }

    __asm {
        popfd
        popad
        jmp [g_originalWardenHandler]  // trampoline
    }
}

__declspec(naked) void WardenPostHandler() {
    __asm {
        pushad
        pushfd
    }

    // Данные УЖЕ расшифрованы!
    ProcessDecryptedWardenData(g_lastDataStore, g_lastReadPos);

    __asm {
        popfd
        popad
        jmp [g_originalReturnAddress]  // возврат к настоящему caller'у
    }
}
```

**Что логируем:**
- Тип пакета (MODULE_INFO, CHEAT_CHECKS_REQUEST, HASH_REQUEST и т.д.)
- Расшифрованные данные пакета
- Результаты анализа запросов проверок

---

### 2.3. SendPacket (0x00632B50)

**Зачем:** Перехватываем все исходящие пакеты клиента, в том числе `CMSG_WARDEN_DATA` (ответы на Warden запросы).

**Calling convention:** Naked hook (нестандартная)

**Stack layout после pushad/pushfd:**
```
s[10] = CDataStore* (указатель на пакет)
```

**CDataStore format для CMSG:**
```
[0x00-0x03]: opcode (4 байта) — например 0x02E7 = CMSG_WARDEN_DATA
[0x04-...]:  payload (данные пакета)
```

**ПРОБЛЕМА:**
Payload в `CMSG_WARDEN_DATA` **зашифрован RC4** Warden модуля. На этом уровне мы видим только размеры и тайминги, не содержимое.

**РЕШЕНИЕ:**
Используем модуль `warden_rc4.cpp`, который:
1. Сканирует память процесса в поисках RC4 S-box (таблицы состояния шифра)
2. Клонирует все найденные состояния S-box **ДО** того как handler зашифрует CMSG
3. В `SendPacket` пытается расшифровать payload каждым клонированным состоянием
4. Проверяет структурную валидность расшифрованных данных (не только первый байт)

**Типы CMSG_WARDEN_DATA:**
- `MODULE_MISSING` (0x00) — 1 байт
- `MODULE_OK` (0x01) — 1 байт
- `CHEAT_CHECKS_RESULT` (0x02) — 7+ байт (заголовок + результаты проверок)
- `HASH_RESULT` (0x03) — 21 байт (1 байт тип + 20 байт SHA1 хеш)
- `MEM_CHECKS_RESULT` (0x04) — переменная длина

**ВАЖНО:**
НЕ отключаем этот хук в `WardenPreHandler`! CMSG ответ отправляется **ВО ВРЕМЯ** выполнения SMSG handler'а (синхронно).

**Что логируем:**
- Опкод пакета
- Для CMSG_WARDEN_DATA: расшифрованный payload (если удалось)
- Размер и время отправки

---

### 2.4. ARC4::Process (0x00774EA0)

**Зачем:** Диагностика. Видим когда и какие данные шифруются/расшифровываются.

**ВАЖНО:** Это **SESSION HEADER CIPHER**, НЕ Warden payload cipher!

**Что шифруется:**
- **CMSG заголовки:** 6 байт (2 байта размер + 4 байта опкод)
- **SMSG заголовки:** 4 байта (2 байта размер + 2 байта опкод)

**Во время Warden обработки:**
- Первый вызов (`len=6`, main thread) — шифрование заголовка `CMSG_WARDEN_DATA`
- Вызовы от Warden модуля (отдельный TID) — небольшие операции (`len=2`)

**Warden payload RC4:**
Находится **ВНУТРИ** Warden модуля (бинарный blob загружаемый от сервера). Этот хук НЕ перехватывает шифрование Warden данных. Для этого используется `warden_rc4.cpp` (сканирование S-box в памяти).

**Что логируем:**
- Длину данных
- Thread ID вызвавшего потока
- Адрес `this` (структура ARC4)
- Время вызова

**Полезность:**
Помогает понять когда происходит шифрование/расшифрование, различить SESSION cipher от Warden cipher.

---

## 3. Return-Address Hijack (подробно)

Это ключевая техника для обхода проблемы зашифрованных данных в `SMSG_WARDEN_DATA`.

### Шаг за шагом:

**Шаг 1: WardenPreHandler вызывается**
```
Stack (ESP):
+0x00: ret addr (к caller'у, кто вызвал handler)  <-- это мы подменим
+0x04: 0
+0x08: 0x2E6
+0x0C: ECX
+0x10: CDataStore*
```

**Шаг 2: Сохраняем данные**
```cpp
CDataStore* ds = *(CDataStore**)(esp + 0x10);
g_lastDataStore = ds;
g_lastReadPos = ds->readPos;  // обычно 2 (опкод прочитан)
```

**Шаг 3: Подменяем return address**
```cpp
void* originalRet = *(void**)(esp + 0x00);
g_originalReturnAddress = originalRet;
*(void**)(esp + 0x00) = (void*)&WardenPostHandler;
```

Теперь stack:
```
+0x00: &WardenPostHandler  <-- подменено!
+0x04: 0
+0x08: 0x2E6
+0x0C: ECX
+0x10: CDataStore*
```

**Шаг 4: Прыгаем в оригинальный handler**
```asm
jmp [g_originalWardenHandler]  ; trampoline от MinHook
```

**Шаг 5: Оригинальный handler работает**
- Расшифровывает данные в `CDataStore->buffer`
- Обрабатывает пакет
- Делает `RET`

**Шаг 6: RET попадает в WardenPostHandler**
Вместо возврата к caller'у, CPU берёт адрес с вершины стека — там наш `&WardenPostHandler`!

**Шаг 7: WardenPostHandler читает данные**
```cpp
void ProcessDecryptedWardenData(CDataStore* ds, uint32_t readPos) {
    ds->readPos = readPos;  // откатываем к началу
    uint8_t* buffer = ds->buffer;
    uint32_t size = ds->size;

    // Теперь buffer содержит РАСШИФРОВАННЫЕ данные!
    uint8_t opcode = buffer[readPos];
    // ... анализируем ...
}
```

**Шаг 8: Возвращаем управление настоящему caller'у**
```asm
jmp [g_originalReturnAddress]
```

### Почему это работает?

Инструкция `RET` делает:
```asm
pop eip     ; берёт адрес с вершины стека и прыгает туда
```

Мы контролируем что лежит на вершине стека (return address), поэтому можем перенаправить выполнение куда угодно.

### Альтернативы (почему они НЕ работают)

**Попытка 1:** Прочитать данные в PreHandler
- НЕ работает: данные зашифрованы

**Попытка 2:** Захукать функцию расшифровки RC4
- НЕ работает: функция расшифровки находится ВНУТРИ Warden модуля, адрес меняется при каждой загрузке

**Попытка 3:** Прочитать данные после вызова оригинального handler'а
- НЕ работает: `CDataStore` может быть уже освобождён или перезаписан

**Return-Address Hijack:** Единственный способ гарантированно получить расшифрованные данные.

---

## 4. CDataStore layout

`CDataStore` — это структура для чтения/записи бинарных данных в WoW клиенте.

### Layout (offsets для x86 build 12340):
```cpp
struct CDataStore {
    void* vtable;          // +0x00 (4 байта) — указатель на виртуальную таблицу
    uint8_t* buffer;       // +0x04 (4 байта) — указатель на данные
    // ... (неизвестные поля)
    uint32_t size;         // +0x10 (4 байта) — размер данных
    uint32_t readPos;      // +0x14 (4 байта) — текущая позиция чтения
    // ...
};
```

### Чтение данных:

```cpp
// Получаем указатель на CDataStore
CDataStore* ds = ...;

// Читаем один байт
uint8_t byte = ds->buffer[ds->readPos];
ds->readPos += 1;

// Читаем uint32_t (4 байта, little-endian)
uint32_t value = *(uint32_t*)(&ds->buffer[ds->readPos]);
ds->readPos += 4;

// Читаем строку (null-terminated)
const char* str = (const char*)(&ds->buffer[ds->readPos]);
ds->readPos += strlen(str) + 1;  // +1 для '\0'
```

### Безопасное чтение:

Всегда проверяем границы перед чтением:
```cpp
if (ds->readPos + sizeof(uint32_t) <= ds->size) {
    uint32_t value = *(uint32_t*)(&ds->buffer[ds->readPos]);
    ds->readPos += 4;
} else {
    LOG(ERROR) << "CDataStore read overflow!";
}
```

### Примеры readPos:

При получении `SMSG_WARDEN_DATA`:
- Опкод пакета (0x2E6) уже прочитан — `readPos = 2`
- Первый байт Warden опкода на позиции `buffer[2]`

После расшифровки:
- `buffer[2]` = Warden opcode (0x00..0x05)
- `buffer[3]` = первый байт payload
- и т.д.

---

## 5. SEH (__try/__except)

**SEH** (Structured Exception Handling) — механизм Windows для обработки исключений на уровне ОС.

### Зачем в проекте?

При анализе Warden мы часто читаем память по указателям, которые могут быть невалидными. Вместо краша используем SEH для безопасного чтения.

### Пример использования:

```cpp
bool SafeReadMemory(void* address, void* buffer, size_t size) {
    __try {
        memcpy(buffer, address, size);
        return true;
    }
    __except(EXCEPTION_EXECUTE_HANDLER) {
        // Access violation — адрес невалиден
        return false;
    }
}
```

### КРИТИЧЕСКОЕ ОГРАНИЧЕНИЕ MSVC:

**SEH нельзя использовать в функциях с C++ объектами!**

Это НЕ работает:
```cpp
void BadFunction() {
    std::string str = "test";  // C++ объект
    __try {
        // ...
    }
    __except(EXCEPTION_EXECUTE_HANDLER) {
        // ОШИБКА КОМПИЛЯЦИИ: "cannot use __try in functions that require object unwinding"
    }
}
```

### Решение: Вынести в отдельную helper-функцию

```cpp
// Helper без C++ объектов
static bool SafeReadCDataStoreImpl(CDataStore* ds, uint8_t* outBuffer, uint32_t size) {
    __try {
        if (ds->readPos + size > ds->size) {
            return false;
        }
        memcpy(outBuffer, &ds->buffer[ds->readPos], size);
        return true;
    }
    __except(EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Основная функция может использовать C++ объекты
void ProcessWardenData() {
    std::string logMessage;  // OK
    uint8_t buffer[1024];

    if (SafeReadCDataStoreImpl(g_lastDataStore, buffer, 100)) {
        logMessage = "Success";
    } else {
        logMessage = "Failed";
    }
}
```

### Где используется в проекте:

1. **SafeReadCDataStore** — чтение из CDataStore с защитой от invalid pointer
2. **SafeReadBytes** — чтение произвольной памяти процесса
3. **warden_rc4.cpp** — сканирование памяти для поиска S-box (могут быть protected regions)
4. **warden_scan.cpp** — анализ памяти Warden модуля

### Типы исключений:

```cpp
__except(EXCEPTION_EXECUTE_HANDLER) {
    // Обрабатываем ВСЕ исключения
}

__except(GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
    // Обрабатываем только access violation, остальное пробрасываем дальше
}
```

### Получение информации об исключении:

```cpp
__except(EXCEPTION_EXECUTE_HANDLER) {
    DWORD code = GetExceptionCode();
    if (code == EXCEPTION_ACCESS_VIOLATION) {
        LOG(WARNING) << "Access violation at address";
    } else if (code == EXCEPTION_INT_DIVIDE_BY_ZERO) {
        LOG(ERROR) << "Division by zero";
    }
}
```

---

## Итого

Проект использует **4 хука**:

1. **FrameScript_Execute** — простой __cdecl хук, логирование Lua кода
2. **SMSG_WARDEN_DATA** — сложный naked хук + return-address hijack для чтения расшифрованных данных
3. **SendPacket** — naked хук для перехвата исходящих пакетов + попытка расшифровки через клонированные RC4 states
4. **ARC4::Process** — диагностический хук для отслеживания session cipher (НЕ Warden cipher)

Ключевые техники:
- **Return-Address Hijack** — единственный способ получить расшифрованные Warden данные
- **SEH в helper-функциях** — безопасное чтение памяти без крашей
- **CDataStore** — структура для чтения бинарных пакетов (buffer + size + readPos)
- **Naked functions** — для нестандартных calling conventions

Дополнительно, **warden_rc4_hook.cpp** устанавливает до 4 хуков на RC4 PRGA функции **внутри** Warden модуля (не WoW.exe). Эти хуки захватывают CMSG plaintext ДО шифрования и являются основным методом расшифровки CMSG.

Все хуки работают вместе для полного анализа Warden системы античита.
