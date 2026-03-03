# Глубокое исследование: Архитектура, Протоколы и Реализация MEM_CHECK Spoofing для Warden в WoW 3.3.5a

## Введение в защитные механизмы клиента World of Warcraft 3.3.5a

Экосистема World of Warcraft (WoW) версии 3.3.5a (Build 12340), выпущенная в эпоху Wrath of the Lich King, представляет собой классический пример архитектуры «клиент–сервер» конца 2000-х, где доверие к клиенту минимально, но неизбежно.  
Для повышения защищённости был внедрен Warden — динамически подгружаемый античит-модуль для проверки целостности памяти процесса, обнаружения стороннего ПО, предотвращения манипуляций с протоколом.

Для исследователей ИБ и разработчиков приватных серверов (AzerothCore, TrinityCore) понимаение механизма работы Warden критично.

Основной механизм — сканирование памяти, особенно через MEM_CHECK:  
Сервер запрашивает байты памяти по определённому адресу и сверяет с эталоном — любые инструменты типа inline-hooking или software breakpoints (JMP/INT 3) тут же выявляются контрольной суммой либо обнаружением сырых байт.

**В этом отчёте:** детальный анализ протокола Warden, криптографии, методика обхода MEM_CHECK через Shadow Copy (ответ с диска, а не ОЗУ) — позволяет сделать хуки невидимыми для античита сервера.

---

## 1. Архитектурный анализ системы Warden

Система Warden — не часть WoW.exe напрямую, а полиморфный механизм, загружающийся с сервера в рантайме:
- позволяет обновлять логику без патча клиента,
- присутствует в памяти только во время сессии.

### 1.1. Жизненный цикл Warden

1. **Инициализация (Handshake):** сервер присылает пакет с seed (зерно), используется для генерации ключей.
2. **Загрузка модуля:** сервер передаёт клиенту бинарник Warden; загружается вручную (Manual Map), не через LoadLibrary, модуль не виден извне.
3. **Активация и криптография:** модуль расшифровывается, проверяется MD5+RSA, передаёт управление в entry point.
4. **Проверки (Challenge–Response цикл):** периодические запросы сервером (WARDEN_SMSG_CHEAT_CHECKS_REQUEST), модуль проверяет — шлёт результат (WARDEN_CMSG_CHEAT_CHECKS_RESULT).

### 1.2. Классификация проверок

| Тип (Hex) | Идентификатор    | Описание                                | Размер запроса         |
|-----------|:-----------------|:-----------------------------------------|:-----------------------|
| 0xF3      | MEM_CHECK        | Проверка байт по адресу памяти           | 1+4+1 байт             |
| 0xB2      | PAGE_CHECK_A     | SHA-1 хеш страницы памяти (4КБ)          | 4+20+4+1 байт          |
| 0xBF      | PAGE_CHECK_B     | Как A, но только страницы MZ/PE          | 4+20+4+1 байт          |
| 0x98      | MPQ_CHECK        | Проверка целостности архива .MPQ         | 1 байт (индекс)        |
| 0xD9      | MODULE_CHECK     | Поиск внедрённых DLL                     | 4+20 байт              |
| 0x8B      | LUA_EVAL_CHECK   | Выполнение Lua–кода                      | Переменный            |

**Наибольшую угрозу для скрытия хуков представляют MEM_CHECK (0xF3) и PAGE_CHECK.**

---

## 2. Криптографический слой и транспортный протокол

В 3.3.5a используется потоковый шифр RC4 ко всей полезной нагрузке Warden–пакетов (исключая WoW-пакет заголовка).

### 2.1. Алгоритм генерации ключей (SHA1 XOR-Fold)

Ключ RC4 не передаётся напрямую, формируется через seed обмен:

1. **Формирование входного буфера:** Конкатенация ServerSeed+ClientSeed (по 16 байт), иногда добавляется ModuleKey.
2. **Хеширование:** `H = SHA1(Buffer)` — результат 20 байт.
3. **Свертка (XOR-Fold):** Хеш до 16 байт (отсекаем или XOR последних 4 байт с началом).

**Критическая уязвимость:** если мы расшифруем пакет "в лоб", RC4 сдвинется и модуль Warden, попытавшись расшифровать тот же пакет — выйдет из синхронизации, и соединение разорвётся.  
**Решение:** нужен отдельный Shadow Context RC4, для анализа и формирования spoofed ответа.

---

### 2.2. Структура пакетов SMSG_WARDEN_DATA

- `SMSG_WARDEN_DATA (0x2E6)` — сервер→клиент
- `CMSG_WARDEN_DATA (0x2E7)` — клиент→сервер

**После RC4–расшифровки:**
1. Warden Opcode (1 байт), напр, 0x02 (CHEAT_CHECKS_REQUEST)
2. Данные — последовательность проверок, парсинг по длине каждой проверки.

---

## 3. Анатомия проверки MEM_CHECK (0xF3)

Одна запись проверки MEM_CHECK содержит:

| Смещение | Тип        | Имя/Описание                                                       |
|----------|------------|---------------------------------------------------------------------|
| 0x00     | uint8      | Length: длина данных проверки (обычно 7 байт, не включая этот байт) |
| 0x01     | uint8      | Type: 0xF3 (MEM_CHECK)                                             |
| 0x02     | uint8      | ModuleIndex: 0—WoW.exe, другое—DLL                                 |
| 0x03     | uint32     | Offset (смещение, base+offset)                                     |
| 0x07     | uint8      | ByteCount: сколько байт читать                                     |

**Алгоритм:**
- Получить ModuleBase по ModuleIndex.
- Target = BaseAddress + Offset.
- Прочитать ByteCount байт, отправить в результат.

**На сервере:** bytes сравниваются с эталоном через memcmp.

---

## 4. Методология «Теневой Копии» (Shadow Copy)

**Суть:** ответы Warden не из оперативной памяти (там хуки!), а из чистого WoW.exe на диске.

- Warden нельзя "запретить" читать память. Нужно перехватывать и вручную формировать ответ.
- Прямая подмена адреса невозможна изнутри процесса.

### 4.1. Проблема трансляции адресов (RVA vs File Offset)

- **Virtual Address (VA):** абсолютный адрес в памяти (0x00401000)
- **Relative Virtual Address (RVA):** смещение относительно базы (VA – Base). Warden оперирует именно этим.
- **Raw File Offset:** смещение от начала файла на диске.

На диске/в памяти различные выравнивания секций. Читать просто по Offset нельзя — требуется PE-парсер.

### 4.2. Алгоритм трансляции адресов

1. **Парсим заголовки:** IMAGE_DOS_HEADER, переходим к IMAGE_NT_HEADERS.
2. **Таблица секций:** Массив IMAGE_SECTION_HEADER.
3. **Поиск секции:** в какую секцию попадает RVA.
4. **Вычисляем дельту:**
   ```
   Δ (delta) = RVA - Section.VirtualAddress
   FileOffset = Section.PointerToRawData + Δ
   ```
5. **Читаем данные по FileOffset из файла.**

### 4.3. Обработка релокаций (Relocations)

В WoW 3.3.5a обычно отключён ASLR — BaseAddress фиксирован (0x00400000), следовательно, Offset = RVA.

---

## 5. Практическая реализация Spoofing-механизма

Ниже представлен детальный план реализации на C++ для внедряемой DLL.

### 5.1. Инициализация и загрузка чистого образа

При загрузке нашей DLL (`DllMain`), мы должны открыть оригинальный файл `WoW.exe` и загрузить его в память для быстрого доступа. Чтение с диска каждый раз создаёт задержки, которые могут быть обнаружены через TIMING_CHECK.

```cpp
#include <windows.h>
#include <vector>
#include <fstream>
#include <iostream>

// Структура для хранения данных о секциях
struct SectionInfo {
    uint32_t virtualAddr;
    uint32_t virtualSize;
    uint32_t rawOffset;
};

class ShadowImage {
private:
    std::vector<uint8_t> fileBuffer;
    std::vector<SectionInfo> sections;
    PIMAGE_NT_HEADERS ntHeaders;
public:
    bool Initialize(const std::wstring& path) {
        // Чтение файла в буфер
        std::ifstream file(path, std::ios::binary | std::ios::ate);
        if (!file.is_open()) return false;
        size_t fileSize = file.tellg();
        file.seekg(0, std::ios::beg);
        fileBuffer.resize(fileSize);
        file.read((char*)fileBuffer.data(), fileSize);

        // Парсинг PE заголовков
        PIMAGE_DOS_HEADER dosHeader = (PIMAGE_DOS_HEADER)fileBuffer.data();
        if (dosHeader->e_magic!= IMAGE_DOS_SIGNATURE) return false;
        ntHeaders = (PIMAGE_NT_HEADERS)(fileBuffer.data() + dosHeader->e_lfanew);
        if (ntHeaders->Signature!= IMAGE_NT_SIGNATURE) return false;
        // Сохранение информации о секциях
        PIMAGE_SECTION_HEADER secHeader = IMAGE_FIRST_SECTION(ntHeaders);
        for (int i = 0; i < ntHeaders->FileHeader.NumberOfSections; i++) {
            SectionInfo info;
            info.virtualAddr = secHeader[i].VirtualAddress;
            info.virtualSize = secHeader[i].Misc.VirtualSize;
            info.rawOffset = secHeader[i].PointerToRawData;
            sections.push_back(info);
        }
        return true;
    }
    bool ReadBytes(uint32_t rva, size_t length, std::vector<uint8_t>& out) {
        // Поиск секции
        for (const auto& sec : sections) {
            if (rva >= sec.virtualAddr && rva < sec.virtualAddr + sec.virtualSize) {
                if (rva + length > sec.virtualAddr + sec.virtualSize) return false;
                uint32_t offsetInSection = rva - sec.virtualAddr;
                uint32_t fileOffset = sec.rawOffset + offsetInSection;
                if (fileOffset + length > fileBuffer.size()) return false;
                out.resize(length);
                memcpy(out.data(), &fileBuffer[fileOffset], length);
                return true;
            }
        }
        // Если RVA не в секции (например, в заголовке), можно читать напрямую,
        // если RVA < SizeOfHeaders.
        return false;
    }
};
```

---

### 5.2. Перехват трафика Warden

Необходимо поставить хук на функцию обработки входящих пакетов.  
В 3.3.5a это часто `NetClient::ProcessMessage` или функция диспетчеризации опкодов.

Основные шаги:

1. **Расшифровка (Shadow Context):** используем копию ключа RC4.
2. **Анализ:** если первый байт — `0x02` (CHEAT_CHECKS_REQUEST), начинаем подмену.
3. **Обработка потока проверок:**
   - Читаем длину, затем тип.
   - Для MEM_CHECK (0xF3):
     - Извлекаем Offset и Length.
     - `ShadowImage::ReadBytes(Offset, Length)` — получаем "чистые" байты.
   - Для PAGE_CHECK:
     - Читаем 4096 байт из ShadowImage, считаем SHA-1, пишем хеш в ответ.
   - Для остальных: либо эмулируем, либо возвращаем пустое значение.
4. **Формируем ответ:**
   - Собираем результаты всех проверок в пакет `CMSG_WARDEN_DATA`.
   - Шифруем через RC4 (shadow context), отправляем серверу.
5. **Блокировка:** оригинальный код клиента не должен получить этот пакет! Иначе случится рассинхронизация RC4.

---

### 5.3. Проблема RC4 Key Initialization

**Как получить ключ для Shadow Context?**
- Самый надёжный способ — перехватить генерацию ключа (функция, имитирующая `SHA1_Final` или `SARC4_SetKey`):

```cpp
// Псевдокод хука установки ключа
void __stdcall Hooked_SARC4_SetKey(uint8_t* key, int len) {
    MyShadowRC4.Init(key, len);
    // Оригинал, если не полностью эмулируем Warden
    Original_SARC4_SetKey(key, len);
}
```

В полной эмуляции можно самому реализовать handshake, генерировать ключ и игнорировать загрузку бинарника, но чаще используется "Man-in-the-Middle" внутри процесса: даём модулю загрузиться, но фильтруем его входящие пакеты.

---

## 6. Серверная валидация и риски обнаружения

Понимание серверной логики проверки MEM_CHECK гарантирует избежание ошибок.

```cpp
// Пример логики сервера (AzerothCore)
if (check->Type == MEM_CHECK) {
    if (memcmp(receivedData, expectedData, check->Length) != 0) {
        // Ошибка проверки!
        Penalty(player);
    }
}
```

**Важно:** версия вашего `WoW.exe` на диске должна БИТ-В-БИТ совпадать с эталоном на сервере (Build 12340).  
Иначе — немедленный бан.

---

### 6.1. Тайминг-атаки (Timing Checks)

Иногда (редко) сервер может измерять интервал между запросом и ответом:
- ОЗУ доступ — наносекунды,
- HDD/SSD — миллисекунды.

**Решение:** Полная предзагрузка WoW.exe в ОЗУ (см. ShadowImage::Initialize).

### 6.2. Self-Check

Сервер может проверить память не WoW.exe, а самого Warden–модуля либо область с хуком:
- Если это системная DLL — нужно иметь оригинал DLL с диска.
- Если это динамически загруженный модуль Warden, надо сохранять его бинарник при загрузке и отдавать bytes из файла.

### 6.3. Контекстные проверки

Иногда через MEM_CHECK проверяют динамические переменные (.data), а не только код (.text).  
Shadow Copy подходит только для кода — если адрес внутри writeable section, надо отвечать из реальной памяти, иначе — из Shadow Copy.

---

## 7. Расширенные данные и сравнительный анализ

### 7.1. Структура базы данных серверных проверок (warden_checks)

| Поле    | Тип данных | Назначение                          |
|---------|------------|-------------------------------------|
| id      | smallint   | Уникальный ID проверки              |
| type    | tinyint    | Тип проверки                        |
| input   | blob       | Данные запроса (смещение, длина)    |
| result  | blob       | Ожидаемый результат (эталонные байты или хеш) |
| comment | varchar    | Описание                            |

**Верификация на сервере** — простое бинарное сравнение (memcmp).

---

### 7.2. Сравнение методов обхода

| Метод             | Сложность     | Надёжность       | Примечание                               |
|-------------------|--------------|------------------|------------------------------------------|
| Снятие хука       | Высокая      | Низкая           | Требует идеальной синхронизации потоков, риск race condition |
| Shadow Copy       | Средняя      | Высокая          | Не требует снятия хуков, подходит для всех статических проверок |
| Полная эмуляция   | Очень высокая| Максимальная     | Требует полного реверса, сложно поддерживать при обновлениях |

---

## 8. Заключение

Реализация подмены MEM_CHECK для Warden 3.3.5a — классическая задача защитных исследований игровых протоколов.  
**Метод "Теневой копии" (Shadow Copy):**
- Эффективен против статических проверок (код, rdata),
- Не требует снятия хуков,
- Позволяет полностью изолировать чит от античита при правильной работе с RC4/PE.

---

## Список использованных технических источников

1. **Реализация WardenWin и структура пакетов в AzerothCore**  
   [https://www.scribd.com/document/582975372/WardenWin](https://www.scribd.com/document/582975372/WardenWin)
2. **Warden.h File Reference — AzerothCore Doxygen**  
   [https://www.azerothcore.org/doxygen/d7/dc7/Warden_8h.html](https://www.azerothcore.org/doxygen/d7/dc7/Warden_8h.html)
3. **RC4 Encryption Algorithm Stream Ciphers Defined — Okta**  
   [https://www.okta.com/identity-101/rc4-stream-cipher/](https://www.okta.com/identity-101/rc4-stream-cipher/)
4. **RC4 — Wikipedia**  
   [https://en.wikipedia.org/wiki/RC4](https://en.wikipedia.org/wiki/RC4)
5. **WoWTools/src/WoWPacketViewer/Parsers/Warden/SMSG_WARDEN_DATA.cs — GitHub**  
   [https://github.com/tomrus88/WoWTools/blob/master/src/WoWPacketViewer/Parsers/Warden/SMSG_WARDEN_DATA.cs](https://github.com/tomrus88/WoWTools/blob/master/src/WoWPacketViewer/Parsers/Warden/SMSG_WARDEN_DATA.cs)
6. **[3.3.5]-AccLeiTo(AccLua) Fix script(Warden) #26053 — GitHub**  
   [https://github.com/TrinityCore/TrinityCore/issues/26053](https://github.com/TrinityCore/TrinityCore/issues/26053)
7. **Índice del espacio — TrinityCore — Confluence**  
   [https://trinitycore.atlassian.net/wiki/spaces/tc/pages/2884304920/page+index](https://trinitycore.atlassian.net/wiki/spaces/tc/pages/2884304920/page+index)
8. **mangos_warden/src/game/WardenMac.cpp at master — GitHub**  
   [https://github.com/tomrus88/mangos_warden/blob/master/src/game/WardenMac.cpp](https://github.com/tomrus88/mangos_warden/blob/master/src/game/WardenMac.cpp)
9. **Kick/Ban when Warden-Checks get interrupted · Issue #17453 — GitHub**  
   [https://github.com/azerothcore/azerothcore-wotlk/issues/17453](https://github.com/azerothcore/azerothcore-wotlk/issues/17453)
10. **Wiki: Warden Modules — SkullSecurity Blog**  
    [https://www.skullsecurity.org/wiki/Warden_Modules](https://www.skullsecurity.org/wiki/Warden_Modules)
