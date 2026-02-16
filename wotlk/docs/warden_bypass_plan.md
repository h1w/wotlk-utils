# Warden Bypass — Детальный план реализации

Система перехвата и анализа команд Warden (античит) для WoW 3.3.5a (Build 12340).
Работа ведётся исключительно на стороне клиента через инжектированную DLL.

---

## Архитектурный обзор

```
Сервер (недоступен)
   │
   ├── SMSG_WARDEN_DATA (opcode 0x2E8) ──► [Фаза 2] Hook 0x007DA850
   │     ├── MEM_CHECK (0xF3)            ──► [Фаза 3] Shadow Copy spoofing
   │     ├── PAGE_CHECK                  ──► [Фаза 5] VirtualProtect evasion
   │     ├── LUA_EVAL                    ──► [Фаза 4] Response spoofing
   │     ├── DRIVER_CHECK
   │     └── MPQ_CHECK
   │
   └── FrameScript_Execute              ──► [Фаза 1] Hook 0x00819210 (мониторинг)
```

**Зависимости между фазами:**
- Фаза 1 — независимая, можно начинать сразу
- Фаза 2 — независимая от Фазы 1, но логически следующая
- Фаза 3 — требует Фазу 2 (нужен парсинг MEM_CHECK из Warden-пакета)
- Фаза 4 — требует Фазу 1 + Фазу 2
- Фаза 5 — требует Фазу 2

---

## Фаза 1: Мониторинг FrameScript_Execute

### Цель
Перехватить **все** вызовы Lua-движка WoW для логирования исполняемого кода.
Это даёт полную видимость того, какие Lua-скрипты выполняет клиент — включая
серверные `LUA_EVAL` запросы от Warden.

### Техническая информация

```
Функция:    FrameScript::Execute
Адрес:      0x00819210
Конвенция:  __cdecl
```

Сигнатура (реверс-инжиниринг, build 12340):
```cpp
// code     — Lua-код для исполнения
// filename — идентификатор источника (для ошибок Lua)
//            "@Interface\FrameXML\UIParent.lua", "***", etc.
// unused   — всегда 0
void __cdecl FrameScript_Execute(const char* code, const char* filename, int unused);
```

### Что логируем
- `filename` — позволяет определить источник вызова
- `code` — сам Lua-код (для LUA_EVAL от Warden это будет строка проверки)
- Timestamp — для корреляции с Warden-пакетами (Фаза 2)

### Фильтрация
Клиент вызывает `FrameScript_Execute` **сотни раз в секунду** (UI, аддоны, фреймы).
Полное логирование завалит лог. Стратегия:
- По умолчанию: логировать только вызовы с подозрительным `filename` (не начинается с `@Interface`)
- Режим verbose: логировать всё (включается флагом / командой в консоль)
- Warden-специфичные LUA_EVAL обычно имеют filename `"***"` или пустую строку

### Библиотека хуков
**MinHook** (через vcpkg, пакет `minhook:x86-windows`):
- Легковесная, x86/x64
- Trampoline-based inline hooking
- Потокобезопасная установка/снятие

### Создаваемые файлы
```
wotlk/
  hooks/
    hooks.h        — API: Initialize() / Shutdown()
    hooks.cpp      — MinHook init + FrameScript_Execute detour
```

### Изменения в существующих файлах
- `dllmain.cpp` — вызов `hooks::Initialize()` после логгера, `hooks::Shutdown()` перед cleanup
- `wotlk.vcxproj` — добавить hooks/*.h и hooks/*.cpp
- `wotlk.vcxproj.filters` — добавить фильтр "hooks"

### Алгоритм реализации
1. `vcpkg install minhook:x86-windows`
2. Создать `hooks/hooks.h` с namespace `hooks { bool Initialize(); void Shutdown(); }`
3. В `hooks.cpp`:
   - Typedef оригинальной функции
   - Глобальный указатель на оригинал (trampoline)
   - Detour-функция: лог параметров → вызов оригинала
   - `Initialize()`: `MH_Initialize()` → `MH_CreateHook()` → `MH_EnableHook()`
   - `Shutdown()`: `MH_DisableHook()` → `MH_Uninitialize()`
4. Интеграция в `dllmain.cpp`

### Риски
- **MEM_CHECK** на адрес 0x00819210 обнаружит inline hook (5 байт JMP перезаписаны)
  → Решение: Фаза 3 (Shadow Copy) скроет модификацию
- MinHook изменяет `.text` секцию → Warden видит `PAGE_EXECUTE_READWRITE`
  → MinHook восстанавливает оригинальные page protection после патча

---

## Фаза 2: Перехват SMSG_WARDEN_DATA

### Цель
Перехватить обработчик входящих Warden-пакетов для парсинга всех типов проверок,
которые сервер отправляет клиенту.

### Техническая информация

```
Обработчик:  SMSG_WARDEN_DATA handler
Opcode:      0x2E8
Адрес:       0x007DA850
```

**Важно:** Hook ставится **после** RC4-дешифрации пакета. На этом адресе данные уже
расшифрованы и готовы к парсингу.

### Структура пакета

```
Offset  Size     Field
0x00    1 byte   Command ID (тип команды Warden)
0x01    2 bytes  Payload Size (длина полезной нагрузки, little-endian)
0x03    1 byte   Check ID (тип конкретной проверки)
0x04    N bytes  Check Data (параметры проверки)
```

Пакет может содержать **несколько проверок подряд** — нужна итерация.

### Парсинг по типам проверок

**MEM_CHECK (Check ID = 0xF3):**
```
Offset  Size     Field
+0      1 byte   String length (seed)
+1      N bytes  String (seed)
+N+1    4 bytes  Target address (little-endian)
+N+5    1 byte   Read length
```
→ Логируем: адрес, длину, seed

**PAGE_CHECK:**
```
Offset  Size     Field
+0      4 bytes  Seed (hash)
+4      4 bytes  SHA1 hash (expected)
+8      4 bytes  Target address
+12     1 byte   Length
```
→ Логируем: адрес, длину, ожидаемый хеш

**LUA_EVAL:**
```
Offset  Size     Field
+0      4 bytes  String length
+4      N bytes  Lua code string (UTF-8)
```
→ Логируем: полный Lua-код

**DRIVER_CHECK:**
```
Offset  Size     Field
+0      4 bytes  Seed
+4      20 bytes SHA1 hash
+24     N bytes  Driver name (null-terminated)
```
→ Логируем: имя драйвера

**MPQ_CHECK:**
```
Offset  Size     Field
+0      N bytes  MPQ file path (null-terminated)
```
→ Логируем: путь к файлу

### Определение сигнатуры обработчика
Необходимо дизассемблировать 0x007DA850 чтобы определить:
- Конвенцию вызова (__cdecl / __thiscall / __fastcall)
- Параметры (buffer pointer, length, возможно this-pointer)
- Return type

Предварительная гипотеза (типично для WoW packet handlers):
```cpp
// __thiscall — this = WardenClient instance
void WardenClient::HandleWardenData(uint8_t* data, uint32_t size);
```

### Создаваемые файлы
```
wotlk/
  warden/
    warden_handler.h     — перехватчик пакетов
    warden_handler.cpp
    warden_parser.h      — парсер проверок
    warden_parser.cpp
    warden_types.h       — структуры данных (CheckType enum, WardenCheck struct)
```

### Алгоритм реализации
1. Дизассемблировать 0x007DA850 для определения сигнатуры
2. Создать структуры данных для каждого типа проверки
3. Реализовать парсер пакетов с итерацией по проверкам
4. Поставить hook на обработчик
5. В detour: парсим → логируем → вызываем оригинал
6. Интеграция с hooks::Initialize() / Shutdown()

### Риски
- **RC4 синхронизация**: НЕ модифицировать содержимое пакета, только читать
- **Thread safety**: Обработчик может вызываться из сетевого потока (не main thread)
  → Логирование должно быть потокобезопасным (glog — потокобезопасен)
- **MEM_CHECK** на адрес 0x007DA850 → решается Shadow Copy (Фаза 3)

---

## Фаза 3: Shadow Copy + MEM_CHECK Spoofing

### Цель
Скрыть inline-хуки от MEM_CHECK сканирования Warden. При запросе хеша участка
памяти — возвращать хеш от **оригинальных** байтов (с диска), а не от
модифицированных (с JMP-патчами).

### Техника Shadow Copy

```
1. CreateFileMapping("Wow.exe", PAGE_READONLY)
2. MapViewOfFile → получаем чистую копию .text секции в памяти
3. При MEM_CHECK запросе на адрес X:
   - offset = X - ImageBase
   - Читаем байты из Shadow Copy по offset
   - Вычисляем хеш от чистых байтов
   - Отправляем серверу "правильный" ответ
```

### Покрытие
Shadow Copy защищает от обнаружения хуков, установленных в Фазах 1 и 2:
- `0x00819210` (FrameScript_Execute) — 5+ байт перезаписаны JMP
- `0x007DA850` (SMSG_WARDEN_DATA handler) — 5+ байт перезаписаны JMP

### Trapped Memory
Warden также сканирует "ловушки" — участки памяти, выглядящие как игровые данные
(координаты игрока, Object Manager), но мониторящиеся на предмет внешнего доступа.
Shadow Copy покрывает и этот случай.

### Создаваемые файлы
```
wotlk/
  warden/
    shadow_copy.h        — создание и управление чистой копией
    shadow_copy.cpp
```

### Алгоритм реализации
1. При инициализации: найти путь к Wow.exe через `GetModuleFileName`
2. `CreateFileMapping` с `PAGE_READONLY` (не `READWRITE` — меньше подозрений)
3. `MapViewOfFile` → маппинг всего файла
4. Парсинг PE-заголовка для определения секций (.text, .rdata)
5. При MEM_CHECK:
   - Проверяем, попадает ли адрес в диапазон наших хуков
   - Если да → читаем из Shadow Copy
   - Если нет → читаем из реальной памяти (штатное поведение)
6. Формируем ответный пакет с "чистым" хешем

### Модификация Фазы 2
В `warden_handler.cpp` добавить логику:
- Перехват MEM_CHECK ответа
- Подмена source данных на Shadow Copy
- Формирование ответного пакета

### Риски
- Shadow Copy должна быть **точной копией** — различия в версиях exe приведут к неверному хешу и дисконнекту
- Маппинг файла Wow.exe — Warden может обнаружить через перечисление открытых handles
  → Решение: закрыть handle файла после маппинга, оставить только маппинг view
- Собственная память DLL (код Shadow Copy) тоже может быть целью MEM_CHECK
  → Решение: аллокация через `VirtualAlloc` с `PAGE_READWRITE` (не EXECUTE)

---

## Фаза 4: LUA_EVAL Response Spoofing

### Цель
Подмена результатов выполнения Lua-кода, запрошенного Warden через `LUA_EVAL`.
Warden ищет глобальные переменные ботов и проверяет "taint" (загрязнение) Lua-окружения.

### Что проверяет Warden через LUA_EVAL
Типичные проверки:
- `return BotGlobalVariable` — ищет известные глобальные переменные ботов
- `return type(SuspiciousFunc)` — проверяет наличие подозрительных функций
- Проверка taint: объекты, затронутые кодом аддона, помечаются как "tainted"

### Стратегия подмены
1. Перехватить Lua-код из LUA_EVAL (уже реализовано в Фазах 1-2)
2. Выполнить код в "чистом" контексте или подменить результат
3. Вернуть `nil` / `false` для подозрительных переменных

### Контроль Call Stack
Warden проверяет стек вызовов Lua — все адреса должны быть внутри `.text` секции
WoW.exe. Если в стеке обнаружен адрес из инжектированной DLL — флаг.

Решение:
- Выполнять `FrameScript_Execute` только из контекста main thread (EndScene hook)
- Адрес возврата должен указывать в `.text` секцию WoW.exe

### Taint-Free окружение
- Не регистрировать глобальные Lua-функции с очевидными именами
- Результаты LUA_EVAL подменять на `nil` / `false` / `0`
- Не модифицировать стандартные Lua-таблицы (_G, string, table, etc.)

### Создаваемые файлы
```
wotlk/
  warden/
    lua_eval_spoof.h     — подмена результатов LUA_EVAL
    lua_eval_spoof.cpp
```

### Алгоритм реализации
1. Из парсера Фазы 2 получаем Lua-код из LUA_EVAL проверки
2. Анализируем код: если запрашивает переменные — вернуть nil
3. Модифицируем hook FrameScript_Execute:
   - Если вызов от Warden (определяем по filename / call context)
   - Подставляем "безопасный" результат
4. Формируем ответный пакет с подменёнными данными

### Риски
- Неправильный формат ответа → дисконнект
- Warden может менять проверки между патчами сервера
- Необходимо тестирование на конкретном сервере

---

## Фаза 5: PAGE_CHECK Evasion

### Цель
Скрыть модификации атрибутов страниц памяти от Warden.
MinHook временно ставит `PAGE_EXECUTE_READWRITE` для записи JMP-патча,
затем восстанавливает. Но Warden может проверить в момент, когда страница
ещё не восстановлена (race condition).

### Что проверяет PAGE_CHECK
- `VirtualQuery` на целевой адрес
- Если `Protect` содержит `PAGE_EXECUTE_READWRITE` — подозрительно
- Нормальные секции `.text` имеют `PAGE_EXECUTE_READ`

### Стратегия
1. MinHook уже восстанавливает protection после патча — это уже помогает
2. Дополнительная защита: hook `VirtualQuery` и подменять результат для наших страниц
3. Собственная память DLL — использовать `PAGE_READWRITE` (без EXECUTE)

### Создаваемые файлы
```
wotlk/
  warden/
    page_protect.h       — защита атрибутов страниц
    page_protect.cpp
```

### Алгоритм реализации
1. Hook `VirtualQuery` (kernel32.dll)
2. В detour: если запрос к нашим страницам → вернуть `PAGE_EXECUTE_READ`
3. Для остальных адресов → штатное поведение

### Риски
- Hook на `VirtualQuery` — это hook на kernel32, что само по себе подозрительно
- Warden может использовать `NtQueryVirtualMemory` напрямую (ntdll.dll)
- Альтернатива: не хукать VirtualQuery, а убедиться что MinHook восстанавливает protection мгновенно

---

## Общая структура файлов (все фазы)

```
wotlk/
  dllmain.cpp                        — точка входа (изменён)
  hooks/
    hooks.h                          — [Фаза 1] API хуков
    hooks.cpp                        — [Фаза 1] MinHook + FrameScript detour
  warden/
    warden_types.h                   — [Фаза 2] Enum и структуры
    warden_handler.h                 — [Фаза 2] Перехват пакетов
    warden_handler.cpp               — [Фаза 2]
    warden_parser.h                  — [Фаза 2] Парсер проверок
    warden_parser.cpp                — [Фаза 2]
    shadow_copy.h                    — [Фаза 3] Чистая копия exe
    shadow_copy.cpp                  — [Фаза 3]
    lua_eval_spoof.h                 — [Фаза 4] Подмена LUA_EVAL
    lua_eval_spoof.cpp               — [Фаза 4]
    page_protect.h                   — [Фаза 5] PAGE_CHECK evasion
    page_protect.cpp                 — [Фаза 5]
  logging/
    glog_custom_formatter.hpp        — (существует)
    glog_custom_formatter.cpp        — (существует)
    logger_setup.hpp                 — (существует)
    logger_setup.cpp                 — (существует)
  docs/
    plan.md                          — (существует)
    bypass_lua.txt                   — (существует)
    offsets.txt                      — (существует)
    warden_bypass_plan.md            — ЭТОТ ФАЙЛ
```

## Зависимости (vcpkg)

| Пакет | Triplet | Фаза |
|-------|---------|------|
| `glog` | x86-windows | существует |
| `minhook` | x86-windows | Фаза 1 |

## Порядок реализации

```
Фаза 1 ─────────────────────────────► Фаза 4
                                        ▲
Фаза 2 ──► Фаза 3                      │
       └──► Фаза 5                      │
       └────────────────────────────────┘
```

Рекомендуемый порядок: **1 → 2 → 3 → 4 → 5**
