# Game SDK

SDK-слой для взаимодействия с игрой World of Warcraft 3.3.5a (build 12340). Предоставляет C++ API для чтения данных и выполнения действий.

**Подход**: Hybrid — C++ для чтения памяти (ObjectManager, дескрипторы, vtable вызовы), Lua (через FrameScript_Execute) для игровых действий (каст, движение). Все вызовы — только с main thread.

---

## Оглавление

1. [Структура файлов](#1-структура-файлов)
2. [Типы и перечисления](#2-типы-и-перечисления)
3. [Безопасное чтение памяти](#3-безопасное-чтение-памяти)
4. [ObjectManager](#4-objectmanager)
5. [Lua Bridge](#5-lua-bridge)
6. [Иерархия объектов](#6-иерархия-объектов)
7. [Спеллы](#7-спеллы)
8. [Движение](#8-движение)
9. [Мир](#9-мир)
10. [Фасад](#10-фасад)
11. [Оффсеты](#11-оффсеты)
12. [Паттерны и ограничения](#12-паттерны-и-ограничения)

---

## 1. Структура файлов

```
wotlk/src/game/
├── game.h / game.cpp             — Фасад SDK
├── types.h                       — Базовые типы
├── mem.h / mem.cpp               — SEH-безопасное чтение памяти
├── object_manager.h / .cpp       — ObjectManager traversal
├── lua_bridge.h / lua_bridge.cpp — Lua мост
├── game_object.h / .cpp          — WowObject (базовый класс)
├── unit.h / unit.cpp             — Unit (HP, ауры, каст)
├── local_player.h / .cpp         — LocalPlayer (статы, XP, золото)
├── spell.h / spell.cpp           — Спеллы (каст, кулдаун)
├── movement.h / movement.cpp     — Движение (ClickToMove, facing)
└── world.h / world.cpp           — Мир (зона, LOS, камера)
```

---

## 2. Типы и перечисления (`types.h`)

```cpp
using GUID = uint64_t;                 // 64-bit идентификатор объекта
constexpr GUID GUID_NONE = 0;

struct Vec3 { float x, y, z; };        // 3D координаты
    // Vec3::DistanceTo(other)         — евклидово расстояние 3D
    // Vec3::Distance2D(other)         — расстояние в плоскости XY

enum class ObjectType  { Object, Item, Container, Unit, Player, GameObject, DynObject, Corpse };
enum class PowerType   { Mana, Rage, Focus, Energy, Happiness, Runes, RunicPower };
enum class UnitReaction { Hostile, Unfriendly, Neutral, Friendly, Honored };

namespace UnitFlags { InCombat, Stunned, Pacified, Confused, Fleeing, NotAttackable, ... }
namespace DynFlags  { Lootable, Dead, Tapped, TappedByPlayer }
```

---

## 3. Безопасное чтение памяти (`mem.h / mem.cpp`)

Все функции чтения обёрнуты в SEH (`__try/__except`). При невалидном указателе возвращают 0 / пустую строку вместо краша.

```cpp
namespace game::mem {
    uint32_t    ReadU32(uintptr_t addr);
    uint64_t    ReadU64(uintptr_t addr);
    float       ReadFloat(uintptr_t addr);
    uintptr_t   ReadPointer(uintptr_t addr);
    std::string ReadCString(uintptr_t addr);
    void        ReadBytes(uintptr_t addr, void* out, size_t len);

    // Дескрипторные поля: desc_base + field * 4
    uint32_t    ReadDescU32(uintptr_t descBase, int field);
    uint64_t    ReadDescU64(uintptr_t descBase, int field);
    float       ReadDescFloat(uintptr_t descBase, int field);
}
```

**Паттерн SEH isolation**: каждый `__try` блок вынесен в отдельную `__cdecl` helper-функцию без C++ объектов на стеке (MSVC не позволяет `__try` в функциях с деструкторами).

---

## 4. ObjectManager (`object_manager.h / .cpp`)

ObjectManager — это внутренняя структура WoW для хранения всех игровых объектов (юнитов, игроков, предметов).

```cpp
namespace game::objmgr {
    uintptr_t GetManagerBase();           // *(CurMgrPointer) + ObjMgrOffset
    bool      IsInGame();                 // проверка валидности ObjectManager
    GUID      GetLocalPlayerGUID();       // managerBase + 0xC0
    uintptr_t GetLocalPlayerPtr();        // через ClntObjMgrObjectPtr @ 0x4D4DB0
    uintptr_t GetObjectPtr(GUID guid);    // поиск объекта по GUID

    // Обход всех объектов (linked list: FirstObject → NextObject)
    using EnumCallback = std::function<bool(uintptr_t objPtr)>;
    void EnumObjects(const EnumCallback& cb);  // safety limit = 10000
}
```

**Как работает `EnumObjects`**:
1. Читает `FirstObject` (managerBase + 0xAC)
2. Для каждого объекта вызывает callback(objPtr)
3. Переходит к `NextObject` (objPtr + 0x3C)
4. Останавливается когда ptr == 0 или лимит 10000

---

## 5. Lua Bridge (`lua_bridge.h / .cpp`)

Обёртка для выполнения Lua кода в клиенте WoW и извлечения результатов.

```cpp
namespace game::lua {
    bool Execute(const char* code);               // выполнить Lua код
    bool Executef(const char* fmt, ...);           // printf-стиль

    std::string GetValue(const char* expr);        // tostring(expr) → string
    int         GetInt(const char* expr);           // atoi(GetValue)
    float       GetFloat(const char* expr);         // atof(GetValue)
    bool        GetBool(const char* expr);          // "1" → true
}
```

**Механизм**:
- `Execute()` вызывает `hooks::GetOriginalFrameScriptExecute()` — trampoline на оригинальный FrameScript_Execute (мимо нашего logging hook)
- `GetValue(expr)` делает `Execute("_wt=tostring(expr)")`, затем вызывает `FrameScript_GetText("_wt")` @ 0x819D40 для извлечения результата

---

## 6. Иерархия объектов

```
WowObject (game_object.h)
  ├── GUID, Type, Entry, Scale
  ├── GetPosition() / GetFacing() / GetName()  — через vtable
  └── GetDescU32/U64/Float()  — чтение дескрипторных полей

    └── Unit (unit.h)
          ├── HP / Mana / Power%
          ├── Level, TargetGUID, FactionTemplate, UnitFlags
          ├── GetUnitName()  — vtable + fallback (+0x964 → +0x5C)
          ├── GetReaction(other) / IsHostile / IsFriendly
          ├── HasAura(spellId)
          ├── GetCastingSpellId() / IsCasting()
          └── IsDead() / IsPlayer() / InCombat()

            └── LocalPlayer (local_player.h)
                  ├── GetXP() / GetNextLevelXP()
                  ├── GetCoinage() / GetGold()
                  ├── GetStrength/Agility/Stamina/Intellect/Spirit()
                  ├── GetComboPoints()
                  ├── GetPlayerName()
                  └── HasSpell(spellId)
```

### WowObject

Лёгкая обёртка вокруг `uintptr_t m_ptr`. **Не владеет указателем** — валиден только в текущем кадре (ObjectManager может переместить объекты между кадрами).

**VTable вызовы** (через SEH-safe helpers: `CallVTGetPosition`, `CallVTGetFacing`, `CallVTGetName`):
- `GetPosition()` → vtable[12] (__thiscall, возвращает Vec3* через стек)
- `GetFacing()` → vtable[14] (__thiscall, возвращает float через FPU)
- `GetName()` → vtable[54] (__thiscall, возвращает const char* в EAX)

**Descriptor access**: все поля юнита/игрока хранятся в descriptor array. Адрес: `*(objPtr + 0x08)`. Каждое поле — 4 байта, индексируется номером поля.

### Unit

Расширяет WowObject данными о бое.

**Прямые вызовы функций WoW**:
- `GetReaction(other)` → `UnitReaction @ 0x7251C0` (__thiscall: ECX=this, push otherPtr)
- `HasAura(spellId)` → `HasAuraBySpellId @ 0x7282A0` (__thiscall: ECX=this, push spellId)

**UnitName для NPC** (fallback если vtable GetName не работает):
1. `ptr1 = *(objPtr + 0x964)`
2. `name = *(ptr1 + 0x05C)` → const char*

### LocalPlayer

Расширяет Unit данными игрока. Использует дескрипторные поля из `offsets::fields::PLAYER_*` и глобальные переменные из `offsets::globals::*`.

---

## 7. Спеллы (`spell.h / .cpp`)

```cpp
namespace game::spell {
    bool HasSpell(uint32_t spellId);     // C++ вызов IsSpellKnown @ 0x53C5B0
    bool IsOnCooldown(uint32_t spellId); // Lua: GetSpellCooldown(id)

    bool CastById(uint32_t spellId);     // Lua: CastSpellByID(id)
    bool CastByName(const char* name);   // Lua: CastSpellByName("name")
    bool StopCasting();                  // Lua: SpellStopCasting()
}
```

---

## 8. Движение (`movement.h / .cpp`)

```cpp
namespace game::movement {
    // C++ ClickToMove @ 0x727400 (__thiscall: ECX=playerPtr)
    bool ClickToMove(const Vec3& pos);
    bool ClickToMoveAttack(GUID targetGuid, const Vec3& pos);
    bool ClickToMoveInteract(GUID targetGuid, const Vec3& pos);
    bool StopCTM();

    // C++ SetFacing @ 0x72EA50 (__thiscall: ECX=playerPtr)
    bool SetFacing(float radians);
    bool FacePosition(const Vec3& target);  // вычисляет угол через atan2

    // Lua
    bool Jump();           // JumpOrAscendStart()
    bool StopMoving();     // MoveAndSteerStop()
}
```

**ClickToMove actions** (`offsets::ctm`):
| Action | Значение | Описание |
|--------|----------|----------|
| Move | 0x04 | Движение к точке |
| Interact | 0x06 | Взаимодействие с объектом |
| Attack | 0x0A | Атака цели |
| Stop | 0x0D | Остановка |

---

## 9. Мир (`world.h / .cpp`)

```cpp
namespace game::world {
    std::string GetZoneText();       // текстовое имя зоны
    std::string GetSubZoneText();    // имя подзоны
    uint32_t    GetZoneId();         // числовой ID зоны
    uint32_t    GetMapId();          // ID карты (0=Eastern Kingdoms, 1=Kalimdor, ...)
    std::string GetRealmName();      // имя сервера

    bool IsInGame();                 // ObjectManager валиден
    bool IsLoading();                // экран загрузки/подключения

    // TraceLine @ 0x7A3B70: проверка прямой видимости
    bool HasLineOfSight(const Vec3& start, const Vec3& end);

    Vec3 GetCameraPosition();        // позиция камеры
}
```

---

## 10. Фасад (`game.h / .cpp`)

Единая точка входа для работы с Game SDK.

```cpp
namespace game {
    bool Initialize();    // вызывается из dllmain.cpp после hooks::Initialize()
    void Shutdown();      // вызывается из dllmain.cpp перед hooks::Shutdown()

    std::optional<LocalPlayer> GetLocalPlayer();
    std::optional<Unit>        GetTarget();
    std::optional<Unit>        GetMouseOver();

    std::vector<Unit> GetAllUnits();
    std::vector<Unit> GetUnitsInRange(float maxDist);

    bool SelectTarget(GUID guid);  // CGGameUI_Target @ 0x524BF0
}
```

---

## 11. Оффсеты

Все оффсеты в `src/offsets/functions.h`, организованы в nested namespaces:

| Namespace | Что содержит |
|-----------|-------------|
| `offsets::fn` | Адреса функций (FrameScript_Execute, ClickToMove, TraceLine, UnitReaction, HasAuraBySpellId, IsSpellKnown, ...) |
| `offsets::opcodes` | Opcodes пакетов (SMSG_WARDEN_DATA, CMSG_WARDEN_DATA) |
| `offsets::pe` | PE-адреса (ImageBase, TextStart, TextEnd) |
| `offsets::objmgr` | ObjectManager (CurMgrPointer=0xC79CE0, ObjMgrOffset=0x2ED0, FirstObject=0xAC, NextObject=0x3C, ...) |
| `offsets::globals` | Глобальные переменные (PlayerName=0xC79D18, TargetGUID=0xBD07B0, ZoneText, MapId, ...) |
| `offsets::vtable` | VTable индексы (GetPosition=12, GetFacing=14, GetName=54) |
| `offsets::fields` | Descriptor field indices (OBJECT_ENTRY=0x03, UNIT_HEALTH=0x18, PLAYER_COINAGE=0x492, ...) |
| `offsets::unit` | Структура Unit (CastingSpellId=0xC08, ChanneledSpellId=0xC20, NameOffset1=0x964, NameOffset2=0x05C) |
| `offsets::ctm` | ClickToMove actions (Move=0x04, Interact=0x06, Attack=0x0A, Stop=0x0D) |

---

## 12. Паттерны и ограничения

### SEH isolation (MSVC)
MSVC не позволяет `__try/__except` в функциях с C++ объектами (std::string, std::vector и т.д.). Решение — вынести `__try` в отдельную `__cdecl` helper-функцию:

```cpp
// Helper — нет C++ объектов, только POD
static bool __cdecl CallVTGetPosition(uintptr_t objPtr, uintptr_t fn, Vec3* outPos) {
    __try {
        __asm { mov eax, outPos; push eax; mov ecx, objPtr; call fn }
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// Основная функция — может использовать C++ объекты
Vec3 WowObject::GetPosition() const {
    Vec3 pos;
    // ... получить fn из vtable ...
    CallVTGetPosition(m_ptr, fn, &pos);
    return pos;
}
```

### Main thread only
Все функции Game SDK должны вызываться с main thread WoW. Внутренние функции игры (ClickToMove, FrameScript_Execute, UnitReaction и т.д.) не thread-safe.

### Время жизни объектов
`WowObject` / `Unit` / `LocalPlayer` не владеют указателем. Объект валиден только в текущем кадре — ObjectManager может переместить/удалить объекты между кадрами. Не кешируйте эти объекты.

### Inline ASM
Проект использует MSVC x86 inline `__asm` для вызова функций с нестандартными calling conventions (__thiscall с this в ECX, __cdecl с push/call/add esp). Это работает только в 32-bit MSVC — не переносимо на x64 или другие компиляторы.
