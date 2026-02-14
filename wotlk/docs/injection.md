# DLL Injection в wotlk-utils

## 1. Зачем нужна инжекция

### Проблема
`Wow.exe` — это чужой процесс, запущенный отдельно от нашей программы. У него своё адресное пространство памяти, свои потоки выполнения. Мы не можем просто так "подключиться" к нему и начать выполнять свой код.

### Решение: DLL Injection
**DLL Injection** — техника загрузки нашей DLL (динамической библиотеки) в адресное пространство целевого процесса.

После успешной инжекции:
- Наша DLL загружена в память `Wow.exe`
- Наш код выполняется **внутри** процесса WoW
- Мы имеем доступ ко **всей памяти** процесса (функции, переменные, структуры данных)
- Можем устанавливать хуки на функции WoW
- Можем читать и изменять игровые данные

### Почему именно DLL?
Windows предоставляет API `LoadLibraryA/W` для загрузки DLL в процесс. Это легальный механизм ОС, который мы используем для инжекции.

---

## 2. Как работает injector

Наш `injector.exe` выполняет следующие шаги для загрузки `wotlk.dll` в процесс `Wow.exe`:

### Шаг 1: Поиск процесса Wow.exe

```cpp
DWORD FindProcessByName(const wchar_t* processName) {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    // Перебираем все процессы в системе
    PROCESSENTRY32W entry = { sizeof(PROCESSENTRY32W) };
    Process32FirstW(snapshot, &entry);
    do {
        if (_wcsicmp(entry.szExeFile, processName) == 0) {
            return entry.th32ProcessID;  // Нашли!
        }
    } while (Process32NextW(snapshot, &entry));
    return 0;  // Не нашли
}
```

**Что происходит:**
- `CreateToolhelp32Snapshot` создаёт "снимок" всех процессов в системе
- Перебираем процессы по одному (`Process32FirstW`, `Process32NextW`)
- Сравниваем имя процесса с `"Wow.exe"`
- Возвращаем PID (Process ID) — уникальный номер процесса

### Шаг 2: Проверка архитектуры процесса

```cpp
bool IsProcess32Bit(HANDLE hProcess) {
    BOOL isWow64 = FALSE;
    IsWow64Process(hProcess, &isWow64);

#ifdef _WIN64
    // Мы 64-бит: если isWow64=TRUE, процесс 32-бит
    return isWow64;
#else
    // Мы 32-бит: можем инжектить только в 32-бит
    return true;
#endif
}
```

**Почему важно:**
- WoW 3.3.5a — это **32-битное** приложение
- Нельзя загрузить 64-битную DLL в 32-битный процесс (ошибка 193: BAD_EXE_FORMAT)
- Нельзя загрузить 32-битную DLL в 64-битный процесс
- Поэтому и `injector.exe`, и `wotlk.dll` должны быть **x86** (32-бит)

### Шаг 3: Открытие процесса

```cpp
HANDLE hProcess = OpenProcess(PROCESS_ALL_ACCESS, FALSE, pid);
if (!hProcess) {
    // Ошибка: возможно нет прав (нужны права администратора)
    DWORD err = GetLastError();  // 5 = ACCESS_DENIED
}
```

**Что происходит:**
- `OpenProcess` открывает дескриптор (handle) процесса
- `PROCESS_ALL_ACCESS` — запрашиваем полные права (чтение, запись, создание потоков)
- Если WoW запущен с правами администратора, injector тоже должен быть запущен с правами администратора

### Шаг 4: Выделение памяти в целевом процессе

```cpp
const char* dllPath = "C:\\path\\to\\wotlk.dll";
size_t pathLen = strlen(dllPath) + 1;  // +1 для '\0'

// Выделяем память в Wow.exe
void* remoteMem = VirtualAllocEx(
    hProcess,
    nullptr,           // Windows сам выберет адрес
    pathLen + 1024,    // размер: путь к DLL + место для shellcode
    MEM_COMMIT | MEM_RESERVE,
    PAGE_EXECUTE_READWRITE  // память для кода и данных
);
```

**Что происходит:**
- `VirtualAllocEx` выделяет память **внутри целевого процесса** (Wow.exe)
- Мы выделяем достаточно места для:
  - Пути к DLL (строка)
  - Shellcode (машинный код для загрузки DLL)
  - Возвращаемые значения (hModule, lastError)

### Шаг 5: Запись данных в выделенную память

```cpp
// Записываем путь к DLL
WriteProcessMemory(hProcess, remoteMem, dllPath, pathLen, nullptr);

// Записываем shellcode (машинный код для вызова LoadLibraryExA)
void* shellcodeAddr = (char*)remoteMem + pathLen;
WriteProcessMemory(hProcess, shellcodeAddr, shellcode, sizeof(shellcode), nullptr);
```

**Что происходит:**
- `WriteProcessMemory` записывает данные в память целевого процесса
- Сначала записываем строку с путём к DLL
- Затем записываем shellcode (бинарный код для x86 процессора)

### Шаг 6: Создание удалённого потока

```cpp
HANDLE hThread = CreateRemoteThread(
    hProcess,
    nullptr,           // security attributes
    0,                 // stack size (default)
    (LPTHREAD_START_ROUTINE)shellcodeAddr,  // адрес функции для выполнения
    remoteMem,         // параметр функции (адрес строки с путём к DLL)
    0,                 // флаги (0 = запустить сразу)
    nullptr            // thread ID (не нужен)
);

// Ждём завершения потока
WaitForSingleObject(hThread, INFINITE);
```

**Что происходит:**
- `CreateRemoteThread` создаёт новый поток **внутри целевого процесса**
- Поток начинает выполнение с адреса `shellcodeAddr` (наш shellcode)
- Параметр `remoteMem` передаётся в shellcode (адрес строки с путём к DLL)
- Injector ждёт пока поток завершится (DLL загружена или произошла ошибка)

### Шаг 7: Чтение результата

```cpp
struct RemoteResult {
    HMODULE hModule;     // Handle загруженной DLL (0 если ошибка)
    DWORD lastError;     // Код ошибки GetLastError()
};

RemoteResult result;
void* resultAddr = (char*)remoteMem + pathLen + sizeof(shellcode);
ReadProcessMemory(hProcess, resultAddr, &result, sizeof(result), nullptr);

if (result.hModule == nullptr) {
    std::cout << "Injection failed, error: " << result.lastError << std::endl;
    // 126 = MOD_NOT_FOUND (DLL или её зависимости не найдены)
    // 193 = BAD_EXE_FORMAT (неправильная архитектура)
} else {
    std::cout << "DLL loaded at: 0x" << std::hex << result.hModule << std::endl;
}
```

**Что происходит:**
- Shellcode сохранил результат (`hModule` и `lastError`) в выделенную память
- `ReadProcessMemory` читает результат обратно в наш процесс
- Проверяем успешность инжекции и выводим сообщение

### Шаг 8: Cleanup

```cpp
VirtualFreeEx(hProcess, remoteMem, 0, MEM_RELEASE);
CloseHandle(hThread);
CloseHandle(hProcess);
```

Освобождаем ресурсы (память, дескрипторы).

---

## 3. Shellcode (34 байта x86)

Shellcode — это машинный код (набор инструкций процессора), который мы внедряем в целевой процесс.

### Что делает наш shellcode:

```asm
; Входные данные:
; - dllPathAddr передан в стеке (первый параметр функции потока)
; - resultAddr = адрес для записи результата

; 1. Вызвать LoadLibraryExA
push 0x1100                    ; dwFlags
push 0                         ; hFile (NULL)
push dword ptr [esp + 12]      ; dllPathAddr (первый параметр потока)
call LoadLibraryExA
mov [resultAddr], eax          ; Сохранить hModule

; 2. Получить код ошибки
call GetLastError
mov [resultAddr + 4], eax      ; Сохранить lastError

; 3. Выход
xor eax, eax
ret 4                          ; Очистить стек (stdcall convention)
```

### Флаги LoadLibraryExA: 0x1100

```cpp
dwFlags = LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS
        = 0x0100 | 0x1000
        = 0x1100
```

**LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR (0x0100):**
- Искать зависимости DLL в **директории самой DLL**
- Например, если `wotlk.dll` находится в `C:\WoW\addons\`, Windows будет искать зависимости там же

**LOAD_LIBRARY_SEARCH_DEFAULT_DIRS (0x1000):**
- Искать зависимости в системных директориях (`C:\Windows\System32`, `C:\Windows\SysWOW64` для x86)
- Искать в директории приложения (`Wow.exe`)

### Почему это важно?

**БЕЗ этих флагов (обычный LoadLibraryA):**
- Windows ищет зависимости только в директории `Wow.exe` и системных папках
- Если `wotlk.dll` зависит от `glog.dll`, которая лежит рядом с `wotlk.dll`, Windows её **НЕ найдёт**
- Результат: **Error 126 (MODULE_NOT_FOUND)**

**С флагом LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR:**
- Windows ищет `glog.dll` в той же папке что и `wotlk.dll`
- Всё работает правильно

### Альтернатива (не используется в проекте):

Можно изменить текущую директорию процесса перед `LoadLibraryA`:
```cpp
SetDllDirectoryA("C:\\path\\to\\dll\\folder");
LoadLibraryA("wotlk.dll");
```
Но это влияет на **весь процесс**, что может сломать WoW. Поэтому используем флаги.

### Шестнадцатеричный код shellcode:

```
68 00 11 00 00        push 0x1100
6A 00                 push 0
FF 74 24 0C           push dword ptr [esp + 0Ch]
...
```

Injector встраивает этот машинный код в память `Wow.exe` и запускает через `CreateRemoteThread`.

---

## 4. Выгрузка (eject)

Выгрузка DLL — обратный процесс, но **намного сложнее** чем загрузка.

### Проблема: нельзя вызвать FreeLibrary из injector'а

Если просто сделать:
```cpp
// В injector.exe
CreateRemoteThread(hProcess, ..., FreeLibrary, hModule, ...);
```

Это вызовет **КРАШ**, потому что:
1. `FreeLibrary` вызывает `DllMain(DLL_PROCESS_DETACH)`
2. `DllMain` выполняется под **Loader Lock** (глобальная блокировка загрузчика DLL)
3. Если в `DllMain` попытаться джойнить потоки (`WaitForSingleObject(hThread)`) — **DEADLOCK**
4. Потоки не могут завершиться (они ждут Loader Lock), `DllMain` не может завершиться (ждёт потоки)

### Решение: Named Event + FreeLibraryAndExitThread

**1. В DLL создаём именованное событие:**
```cpp
// В DllMain (DLL_PROCESS_ATTACH)
g_unloadEvent = CreateEventA(nullptr, TRUE, FALSE, "wotlk_unload_event");

// Создаём MainThread который ждёт сигнала
g_mainThread = CreateThread(nullptr, 0, MainThreadFunc, nullptr, 0, nullptr);
```

**2. MainThread ждёт сигнала:**
```cpp
DWORD WINAPI MainThreadFunc(LPVOID) {
    while (true) {
        DWORD wait = WaitForSingleObject(g_unloadEvent, 100);
        if (wait == WAIT_OBJECT_0) {
            // Получен сигнал выгрузки!
            break;
        }
    }

    // Cleanup
    Cleanup();

    // Атомарно выгружаем DLL и завершаем поток
    FreeLibraryAndExitThread(g_hModule, 0);
    return 0;
}
```

**3. Injector сигналит событие:**
```cpp
// В injector.exe
HANDLE hEvent = OpenEventA(EVENT_MODIFY_STATE, FALSE, "wotlk_unload_event");
SetEvent(hEvent);  // Посылаем сигнал
CloseHandle(hEvent);

// Ждём пока DLL исчезнет из списка модулей
for (int i = 0; i < 50; ++i) {  // До 5 секунд
    if (!FindRemoteModule(hProcess, L"wotlk.dll")) {
        std::cout << "DLL unloaded successfully" << std::endl;
        break;
    }
    Sleep(100);
}
```

### Что такое FindRemoteModule?

```cpp
bool FindRemoteModule(HANDLE hProcess, const wchar_t* moduleName) {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    MODULEENTRY32W entry = { sizeof(MODULEENTRY32W) };
    Module32FirstW(snapshot, &entry);
    do {
        if (_wcsicmp(entry.szModule, moduleName) == 0) {
            CloseHandle(snapshot);
            return true;  // DLL всё ещё загружена
        }
    } while (Module32NextW(snapshot, &entry));
    CloseHandle(snapshot);
    return false;  // DLL выгружена
}
```

Перебирает все модули (DLL) загруженные в процесс и ищет нашу DLL.

### Cleanup функция в DLL:

```cpp
void Cleanup() {
    LOG(INFO) << "Starting cleanup...";

    // 1. Отключаем все хуки
    if (g_framescriptHook) {
        MH_DisableHook(MH_ALL_HOOKS);
        MH_Uninitialize();
    }

    // 2. Закрываем shadow copy файла
    if (g_shadowCopyHandle) {
        CloseHandle(g_shadowCopyHandle);
    }

    // 3. Закрываем консоль (если была открыта)
    if (g_consoleAttached) {
        fclose(stdin);   // ВАЖНО: закрыть file handles
        fclose(stdout);
        fclose(stderr);
        FreeConsole();   // Теперь консоль закроется
    }

    // 4. Закрываем логи
    google::ShutdownGoogleLogging();

    LOG(INFO) << "Cleanup complete";
}
```

### FreeLibraryAndExitThread — магическая функция

```cpp
DECLSPEC_NORETURN VOID FreeLibraryAndExitThread(
    HMODULE hLibModule,
    DWORD   dwExitCode
);
```

Эта функция Windows **атомарно** (как одна операция):
1. Выгружает DLL (`FreeLibrary(hLibModule)`)
2. Завершает текущий поток (`ExitThread(dwExitCode)`)

Почему это работает:
- Вызывается **НЕ из DllMain**, а из обычного потока
- Loader Lock не удерживается
- После выгрузки DLL код потока больше не существует в памяти, но это ОК — поток уже завершён

---

## 5. Почему нельзя делать cleanup в DllMain

### Loader Lock

Когда Windows загружает/выгружает DLL, он захватывает глобальную блокировку **Loader Lock**.

```
DllMain вызывается ТАК:
    EnterLoaderLock()        // Захват блокировки
    DllMain(DLL_PROCESS_ATTACH или DLL_PROCESS_DETACH)
    LeaveLoaderLock()        // Освобождение блокировки
```

**Проблема 1: Нельзя джойнить потоки**
```cpp
// В DllMain (DLL_PROCESS_DETACH) — НЕ ДЕЛАЙТЕ ТАК!
BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved) {
    if (fdwReason == DLL_PROCESS_DETACH) {
        WaitForSingleObject(g_workerThread, INFINITE);  // DEADLOCK!
    }
}
```

Почему DEADLOCK:
- `DllMain` удерживает Loader Lock
- `g_workerThread` пытается выйти, его деструкторы C++ объектов могут вызывать функции из других DLL
- Вызов функции из DLL требует Loader Lock
- Поток ждёт Loader Lock, `DllMain` ждёт поток — **DEADLOCK**

**Проблема 2: Нельзя вызывать LoadLibrary/FreeLibrary**
```cpp
// В DllMain — НЕ ДЕЛАЙТЕ ТАК!
FreeLibrary(hSomeDll);  // DEADLOCK (пытается захватить Loader Lock, который уже захвачен)
```

**Проблема 3: Ограничения на API**
В `DllMain` безопасно вызывать только ограниченный набор функций:
- Локальная работа с памятью (malloc, new)
- Базовые системные вызовы
- НЕ безопасно: file I/O, registry, COM, sync primitives (кроме критических секций)

### Правильный подход:

```cpp
// DllMain — МИНИМУМ кода
BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved) {
    if (fdwReason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hinstDLL);  // Оптимизация
        g_hModule = hinstDLL;

        // Создаём событие и поток — это безопасно
        g_unloadEvent = CreateEventA(nullptr, TRUE, FALSE, "wotlk_unload_event");
        g_mainThread = CreateThread(nullptr, 0, MainThreadFunc, nullptr, 0, nullptr);
    }
    return TRUE;
}

// MainThread — ВСЯ логика здесь
DWORD WINAPI MainThreadFunc(LPVOID) {
    // Инициализация (логи, хуки, консоль)
    Initialize();

    // Ожидание сигнала выгрузки
    WaitForSingleObject(g_unloadEvent, INFINITE);

    // Cleanup (отключение хуков, закрытие файлов)
    Cleanup();

    // Выгрузка DLL
    FreeLibraryAndExitThread(g_hModule, 0);
}
```

---

## 6. Типичные ошибки

### Error 126 (MODULE_NOT_FOUND)

**Причина:** Windows не может найти DLL или её зависимости.

**Возможные причины:**
1. Путь к DLL неправильный
2. DLL зависит от других DLL (например `glog.dll`), которые не найдены
3. Не используется флаг `LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR`

**Диагностика:**
```bash
# Используйте Dependency Walker (depends.exe) для просмотра зависимостей
depends.exe wotlk.dll
```

**Решение:**
- Используйте полный абсолютный путь к DLL
- Используйте флаг `0x1100` для `LoadLibraryExA`
- Положите все зависимости рядом с `wotlk.dll`

### Error 193 (BAD_EXE_FORMAT)

**Причина:** Несовпадение архитектуры DLL и процесса.

**Примеры:**
- Загрузка x64 DLL в x86 процесс
- Загрузка x86 DLL в x64 процесс

**WoW 3.3.5a — это x86 (32-бит)**, поэтому:
- `wotlk.dll` должна быть скомпилирована как **x86** (Win32)
- `injector.exe` должен быть скомпилирован как **x86** (Win32)

**Проверка в Visual Studio:**
- Configuration Manager → Active solution platform → **Win32** (НЕ x64)

### Error 5 (ACCESS_DENIED)

**Причина:** Недостаточно прав для доступа к процессу.

**Решение:**
1. Запустите injector с правами администратора
2. Если WoW запущен с правами администратора, injector тоже должен быть

**Запуск с правами администратора:**
- Правой кнопкой на `injector.exe` → "Запуск от имени администратора"
- Или добавить манифест `requireAdministrator` в проект

### Консоль не закрывается при eject

**Причина:** File handles от `freopen_s` (или `AllocConsole`) удерживают консоль.

**Неправильно:**
```cpp
void Cleanup() {
    FreeConsole();  // Консоль НЕ закроется!
}
```

**Правильно:**
```cpp
void Cleanup() {
    // Сначала закрываем file handles
    if (stdin) fclose(stdin);
    if (stdout) fclose(stdout);
    if (stderr) fclose(stderr);

    // Теперь консоль закроется
    FreeConsole();
}
```

### Краш при eject

**Причина 1:** Джойним потоки в `DllMain`
```cpp
// НЕ ДЕЛАЙТЕ ТАК!
if (fdwReason == DLL_PROCESS_DETACH) {
    WaitForSingleObject(g_thread, INFINITE);  // DEADLOCK
}
```

**Решение:** Используйте `FreeLibraryAndExitThread` из отдельного потока.

**Причина 2:** Обращаемся к памяти DLL после `FreeLibrary`
```cpp
// НЕ ДЕЛАЙТЕ ТАК!
void Cleanup() {
    FreeLibraryAndExitThread(g_hModule, 0);
    LOG(INFO) << "Done";  // КРАШ! Код LOG уже выгружен из памяти
}
```

**Решение:** Весь код после `FreeLibraryAndExitThread` не выполняется (функция не возвращает управление).

### Injector завис при ожидании

**Причина:** Удалённый поток не завершается.

**Возможные причины:**
1. Shellcode содержит бесконечный цикл (ошибка в коде)
2. `LoadLibraryExA` зависла (редко, может быть если DLL_PROCESS_ATTACH содержит бесконечный цикл)

**Решение:**
```cpp
// Используйте таймаут вместо INFINITE
DWORD wait = WaitForSingleObject(hThread, 5000);  // 5 секунд
if (wait == WAIT_TIMEOUT) {
    std::cerr << "Remote thread timed out!" << std::endl;
    TerminateThread(hThread, 1);  // Крайняя мера
}
```

---

## Итого

### Процесс инжекции (шаги):
1. Найти PID процесса `Wow.exe`
2. Проверить что это 32-бит процесс
3. Открыть процесс с `PROCESS_ALL_ACCESS`
4. Выделить память в процессе (`VirtualAllocEx`)
5. Записать путь к DLL и shellcode (`WriteProcessMemory`)
6. Создать удалённый поток выполняющий shellcode (`CreateRemoteThread`)
7. Дождаться завершения потока и прочитать результат (`ReadProcessMemory`)
8. Освободить ресурсы

### Процесс выгрузки (шаги):
1. Найти hModule нашей DLL в процессе (`FindRemoteModule`)
2. Открыть именованное событие `"wotlk_unload_event"`
3. Послать сигнал (`SetEvent`)
4. Дождаться пока DLL исчезнет из списка модулей (до 5 секунд)

### Ключевые моменты:
- **Флаг 0x1100** для `LoadLibraryExA` — критически важен для поиска зависимостей
- **FreeLibraryAndExitThread** — единственный безопасный способ самовыгрузки
- **Loader Lock deadlock** — причина почему cleanup нельзя делать в `DllMain`
- **File handles** — нужно закрыть перед `FreeConsole`, иначе консоль не закроется
- **Архитектура x86** — и DLL, и injector должны быть 32-бит для WoW 3.3.5a

### Инструменты для диагностики:
- **Dependency Walker** (depends.exe) — просмотр зависимостей DLL
- **Process Hacker** — просмотр загруженных модулей в процессе
- **DebugView** (Sysinternals) — просмотр `OutputDebugString` сообщений

DLL injection — мощная техника, но требует аккуратности. Одна ошибка = краш всего процесса WoW.
