# wotlk-utils — Документация проекта

## Что это такое

Это проект для исследования WoW 3.3.5a (Wrath of the Lich King). Простыми словами:

- **wotlk.dll** — это файл, который мы "впихиваем" (инжектируем) в процесс игры Wow.exe
- Эта DLL перехватывает важные функции внутри игры и смотрит, что там происходит
- Главная цель — понять, как работает **Warden** (система античита в WoW)
- Всё что происходит — записывается в логи, чтобы потом можно было изучить

**Injector** — это отдельная программа, которая засовывает нашу DLL в игру и потом может её выгрузить.

**Важно:** WoW 3.3.5a — это 32-битная (x86) программа. Поэтому и DLL, и injector должны быть 32-битными. Не 64-битными!

---

## Структура проекта

```
wotlk-utils/
├── injector/           # Программа-инжектор (консольное приложение x86)
│   ├── main.cpp        # Основной код инжектора
│   └── logging/        # Настройка логирования для инжектора
│
├── wotlk/              # DLL, которая инжектируется в Wow.exe
│   ├── dllmain.cpp     # Точка входа DLL (загрузка/выгрузка)
│   ├── hooks/          # Перехватчики функций (используем MinHook)
│   │   ├── framescript_execute.cpp  # Хук выполнения Lua-кода
│   │   ├── warden_handler.cpp       # Хук обработчика пакетов Warden
│   │   ├── send_packet.cpp          # Хук отправки пакетов на сервер
│   │   └── arc4_process.cpp         # Хук RC4 шифрования заголовков
│   │
│   ├── warden/         # Анализ системы Warden
│   │   ├── warden_scan.cpp          # Сканирование модулей (in-memory + RLE unpack + dispatch chain)
│   │   ├── warden_rc4.cpp           # Поиск RC4 S-box и расшифровка (fallback)
│   │   ├── warden_rc4_hook.cpp      # Хук RC4 PRGA внутри модуля (primary)
│   │   ├── warden_module_dump.cpp   # Сохранение модулей на диск
│   │   ├── warden_shadow_copy.cpp   # Чтение оригинальных байт .text из PE на диске
│   │   ├── warden_checksum.cpp      # Checksum algorithm (SHA1 XOR-fold)
│   │   └── warden_checksum.h        # Заголовок для warden_checksum
│   │
│   ├── logging/        # Настройка логирования для DLL
│   └── docs/           # Документация (ты здесь)
│
└── shared/             # Общий код для injector и wotlk
    └── logging/        # Общая настройка логирования (glog)
```

---

## Структура документации

### Основные документы

| Файл | Описание |
|------|----------|
| **README.md** | Этот файл — точка входа в документацию |
| **architecture.md** | Архитектура всей системы: что за что отвечает, как взаимодействуют части, последовательность событий |
| **injection.md** | Как работает инжекция DLL в процесс игры (LoadLibraryExA, shellcode, x86) |
| **hooks.md** | Все хуки: какие функции перехватываем, зачем, как именно это делается |
| **warden-protocol.md** | Протокол Warden: какие пакеты бывают (SMSG/CMSG), что в них лежит, как их читать |
| **warden-modules.md** | Формат модулей Warden (не PE!), как извлекать типы проверок из кода модуля |
| **rc4-decryption.md** | Как работает RC4 шифрование в Warden, как мы ищем S-box в памяти и расшифровываем CMSG |
| **status.md** | Что уже работает, что НЕ работает, что планируется сделать |

### Справочные материалы

| Папка/Файл | Описание |
|------------|----------|
| **offsets.txt** | Оффсеты функций для билда 12340 |
| **bypass_lua.txt** | Заметки про обход Lua-проверок |
| **warden_bypass_plan.md** | План обхода Warden (оригинальный документ) |
| **reference/** | PDF-статьи, исследования про Warden |
| **plans/** | Оригинальные планы разработки |

### Дампы и скрипты

| Папка | Описание |
|-------|----------|
| **warden_dumps/** | Дампы модулей Warden (encrypted, decrypted, decompressed) |
| **scripts/** | Python-скрипты для анализа модулей (по директориям, см. `scripts/README.md`) |
| **scripts/dispatch/** | Извлечение type IDs из dispatch chains и remap tables |
| **scripts/rc4/** | Анализ RC4 функций в модулях |
| **scripts/analysis/** | Общий анализ модулей |
| **scripts/verification/** | Тестирование и отладка |
| **scripts/module_format/** | RLE-распаковщик и утилиты формата модулей |

---

## Как собрать и запустить

### Требования

- **Visual Studio 2022** (Community edition подойдёт)
- **vcpkg** (менеджер пакетов для C++)
- **glog** (библиотека логирования) — установить через vcpkg для x86-windows и x64-windows
- **MinHook** (библиотека хуков) — установить через vcpkg для x86-windows-static-md

### Установка зависимостей

```bash
# Установить glog для x86 и x64
vcpkg install glog:x86-windows
vcpkg install glog:x64-windows

# Установить MinHook (статическая линковка)
vcpkg install minhook:x86-windows-static-md
```

### Сборка

1. Открой `wotlk-utils.sln` в Visual Studio 2022
2. Выбери конфигурацию **Debug | Win32** (или **Release | Win32**)
   - **Важно:** Именно Win32 (x86), НЕ x64!
3. Собери проект **injector** (правая кнопка → Build)
4. Собери проект **wotlk** (правая кнопка → Build)

### Запуск

1. Запусти `injector.exe` (он будет ждать появления Wow.exe)
2. Запусти WoW 3.3.5a (Wow.exe)
3. Injector автоматически заинжектирует DLL
4. Смотри логи в консоли (цветной вывод)

### Выгрузка DLL

```bash
injector.exe --eject
```

Это безопасно выгрузит DLL из процесса игры (через named event + FreeLibraryAndExitThread).

---

## Быстрый старт для чтения документации

Если ты хочешь разобраться, как всё работает, читай документы в таком порядке:

1. **architecture.md** — начни отсюда, чтобы понять общую картину
2. **injection.md** — как DLL попадает в игру
3. **hooks.md** — что мы перехватываем и зачем
4. **warden-protocol.md** — что такое Warden и какие пакеты он посылает
5. **warden-modules.md** — что внутри модулей Warden и как извлекать информацию
6. **rc4-decryption.md** — как расшифровать зашифрованные пакеты
7. **status.md** — где мы сейчас и что дальше

После этого можно копаться в коде, скриптах, дампах — ты уже будешь понимать, зачем всё это нужно.

---

## Главные особенности (чтобы не забыть)

### Архитектурные решения

- **WoW.exe — это 32-bit процесс**, поэтому все компоненты (DLL, injector) должны быть x86
- Используем `LoadLibraryExA` с флагом `LOAD_WITH_ALTERED_SEARCH_PATH`, потому что Wow.exe не ищет зависимости DLL в её папке
- Выгрузка через **named event** (`wotlk_unload_event`) + `FreeLibraryAndExitThread`, чтобы избежать deadlock (нельзя joinить потоки из DllMain из-за loader lock)

### Хуки

- **FrameScript_Execute** (0x00819210) — перехватывает выполнение Lua-кода
- **SMSG_WARDEN_DATA** (0x007DA850) — перехватывает входящие пакеты Warden
  - **НЕ стандартный __thiscall**, нужен `__declspec(naked)` детур
  - Данные **зашифрованы RC4** на момент входа в хук
  - Используем return-address hijack для пост-обработки (после расшифровки)
- **SendPacket** (0x00632B50) — перехватывает отправку пакетов на сервер
  - CMSG_WARDEN_DATA (0x2E7) тоже зашифрован RC4
- **ARC4::Process** (0x00774EA0) — шифрует/расшифровывает заголовки пакетов
  - **Это НЕ Warden RC4!** Это шифр для заголовков (4-6 байт)
  - Warden RC4 находится внутри модуля Warden в памяти

### Warden RC4

- Warden модуль использует свой RC4 для шифрования payload
- **PRIMARY** (warden_rc4_hook.cpp): хукаем RC4 PRGA функции **внутри** Warden модуля
  - До 4 одновременных хуков (для модулей с несколькими RC4 функциями)
  - 6 поддерживаемых calling conventions (ECX/EDX/EAX/stack-based)
  - Захват plaintext ДО шифрования — 100% success rate
- **FALLBACK** (warden_rc4.cpp): S-box cloning
  - Ищем **S-box** (массив uint8_t[256]) в MEM_PRIVATE памяти
  - Клонируем все найденные состояния S-box перед обработкой SMSG
  - В SendPacket пробуем расшифровать CMSG каждым клоном и проверяем структуру

### Модули Warden

- **НЕ PE-формат!** Это кастомный бинарник (40-байт header, RLE-packed sections, delta relocs)
- Модули загружаются в VirtualAlloc (PAGE_EXECUTE_READWRITE, MEM_PRIVATE)
- **Приоритеты сканирования** для извлечения типов:
  1. **In-memory scan** — сканирование загруженного модуля (primary, самый надёжный)
  2. **RLE-unpacked binary** — распаковка decompressed дампа, затем сканирование
  3. **Raw packed binary** — legacy fallback
  4. **Blind memory scan** — последний resort
- Ищем **dispatch chain** в коде модуля:
  - XOR-anchored scan (паттерн `32 [40-7F] 04`)
  - Remap table supplement (ExtractFromSingleRemap)
  - cmp-cluster + sub-chain fallbacks
- Типы проверок **module-specific** (не совпадают с TC enum)

### Логирование

- Используем **glog** (Google Logging Library)
- Консоль поддерживает ANSI color codes (нужен `ENABLE_VIRTUAL_TERMINAL_PROCESSING`)
- Перед `FreeConsole()` обязательно `fclose(stdin/stdout/stderr)`, иначе консоль не закроется

---

## Для самых нетерпеливых

Хочешь сразу увидеть результат?

1. Собери проект (Debug | Win32)
2. Запусти injector.exe
3. Запусти WoW 3.3.5a
4. Зайди на сервер (любой WotLK 3.3.5a)
5. Смотри в консоль — там будут логи про Warden модули, пакеты, расшифровку

Логи покажут:
- Какой модуль загружен (хеш, размер)
- Какие типы проверок в нём есть (TIMING, LUA, MEM и т.д.)
- Какие пакеты отправляет сервер (MODULE_INFO, HASH_REQUEST, CHEAT_CHECKS_REQUEST)
- Какие ответы мы отправляем (MODULE_OK, HASH_RESULT, CHEAT_CHECKS_RESULT)

Если что-то не работает — читай **status.md**, там описаны известные проблемы.

---

## Контакты и вопросы

Если у тебя вопросы по коду или документации:
- Читай **architecture.md** — там ответы на большинство вопросов
- Смотри логи в консоли — они очень подробные
- Ищи в коде по ключевым словам (Ctrl+Shift+F в Visual Studio)
- Смотри дампы модулей в `warden_dumps/`

Этот проект — для обучения и исследования. Не используй его для читерства на серверах — это против правил и портит игру другим.

---

**Удачи в исследовании Warden!**
