# Bot Architecture — Tool System & Action Queue

> **Status**: Implementation in progress (Phases 1–4 done)
> **Created**: 2026-02-19
> **Last updated**: 2026-02-19
> **Scope**: DLL-side bot framework — инструменты, очередь действий, IPC с внешним контроллером

---

## 1. Концепция

Бот строится по модели **"руки + мозг"**:

- **DLL** (руки) — набор **инструментов** (Tools), которые умеют выполнять конкретные действия в игре. Инструменты исполняются через **очередь действий** (ActionQueue), по одному за раз. DLL не принимает стратегических решений.
- **Внешний контроллер** (мозг) — отдельный процесс (AI, скрипт, ручное управление). Подключается к DLL через IPC (Named Pipe + Protobuf). Отправляет команды: какие инструменты запускать, в каком порядке, когда прерывать.
- **ImGui** — ручной режим. Два виджета: панель инструментов (выбор + параметры + кнопки Execute/Interrupt) и панель очереди (текущее состояние, список ожидающих, кнопки Remove/Clear).

```
┌─────────────────────┐         Named Pipe + Protobuf
│  Внешний контроллер │ ◄──────────────────────────────────┐
│  (AI / скрипт)      │ ──────────────────────────────────► │
└─────────────────────┘                                     │
                                                            │
┌───────────────────────────────────────────────────────────┐
│  DLL (wotlk.dll)                                         │
│                                                           │
│  ┌──────────────┐    thread-safe     ┌────────────────┐  │
│  │  Pipe Server  │ ──command_queue──► │  EndScene tick │  │
│  │  (отд. поток) │                   │  (main thread) │  │
│  └──────────────┘                    └───────┬────────┘  │
│                                              │           │
│  ┌───────────────────────────────────────────┘           │
│  │                                                       │
│  ▼                                                       │
│  ┌──────────────────┐                                    │
│  │   ActionQueue     │ ◄── ImGui кнопки (ручной режим)   │
│  │   (deque<ITool>)  │                                   │
│  └────────┬─────────┘                                    │
│           │                                              │
│           ▼                                              │
│  ┌──────────────────┐                                    │
│  │  current->Tick()  │                                   │
│  └────────┬─────────┘                                    │
│           │                                              │
│           ▼                                              │
│  game::movement  game::spell  game::lua  game::world     │
│  (SDK — низкоуровневые вызовы в игру)                    │
└───────────────────────────────────────────────────────────┘
```

---

## 2. Tool (Инструмент) — базовая абстракция

### 2.1 Жизненный цикл

```
Pending ──Start()──► Running ──Tick()──► Running ──...──► Completed
                         │                                  │
                         └──Abort()──► Cancelled       Failed
```

- **Pending** — инструмент создан, параметры заданы, лежит в очереди, но ещё не запущен.
- **Running** — `Start()` был вызван, `Tick()` вызывается каждый кадр (~60 FPS через EndScene).
- **Completed** — инструмент успешно завершил работу (достиг цели).
- **Failed** — инструмент не смог выполнить задачу (цель недоступна, таймаут, и т.д.).
- **Cancelled** — `Abort()` был вызван извне (Interrupt, Clear, ручная отмена).

### 2.2 Интерфейс ITool

```cpp
enum class ToolType : uint8_t {
    MoveTo,           // Идти в точку (x, y, z)
    Attack,           // Выбрать цель и начать авто-атаку
    CombatRotation,   // Бой: ротация спеллов до смерти цели
    FollowRoute,      // Пройти маршрут (массив waypoints)
    Interact,         // Подойти к объекту и взаимодействовать
    Loot,             // Подлутать текущую цель
    UseSpell,         // Применить конкретный спелл один раз
    Wait,             // Ждать N миллисекунд
    Sequence,         // Композитный: последовательность под-инструментов
};

enum class ToolStatus : uint8_t {
    Pending,
    Running,
    Completed,
    Failed,
    Cancelled,
};

class ITool {
public:
    virtual ~ITool() = default;

    // --- Идентификация ---
    virtual ToolType    GetType() const = 0;
    virtual const char* GetName() const = 0;   // "Move To", "Combat Rotation", ...

    // --- Состояние ---
    virtual ToolStatus  GetStatus() const = 0;

    // --- Жизненный цикл ---
    virtual void Start() = 0;    // Вызывается один раз при переходе Pending → Running
    virtual void Tick() = 0;     // Вызывается каждый кадр пока Running
    virtual void Abort() = 0;    // Прервать. Должен корректно остановить движение/каст/и т.д.

    // --- UI ---
    // NOTE: RenderParams() убран из ITool — UI рендерится напрямую в overlay.cpp через TabBar.
    virtual std::string Describe() const = 0;  // Человекочитаемое описание: "MoveTo (-8900, 500, 100)"
};
```

### 2.3 Конкретные инструменты

#### MoveTo
- **Параметры**: `Vec3 target`
- **Start()**: вызывает `game::movement::ClickToMove(target)`
- **Tick()**: проверяет `player.GetPosition().DistanceTo(target) < threshold` → Completed. Если застрял — повторный ClickToMove или Failed по таймауту.
- **Abort()**: `game::movement::StopCTM()` + `game::movement::StopMoving()`
- **ImGui RenderParams()**: 3 float-поля (x, y, z) + кнопка "Use current pos" (заполняет текущими координатами игрока)

#### Attack
- **Параметры**: `GUID targetGuid` ИЛИ режим `NearestHostile`
- **Start()**: `game::SelectTarget(guid)` + `game::movement::ClickToMoveAttack(guid, targetPos)`
- **Tick()**: проверяет что цель выбрана и персонаж в бою. Если цель мертва → Completed. Если цель потеряна → Failed.
- **Abort()**: `game::movement::StopCTM()`

#### CombatRotation
- **Параметры**: ротация (приоритетный список спеллов с условиями) — загружается из внешней базы знаний / конфига
- **Start()**: проверяет наличие цели, начинает ротацию
- **Tick()**: итерирует по списку спеллов сверху вниз, проверяет условие каждого (HP% цели, свой HP%, кулдаун, аура) → кастует первый подходящий. Если цель мертва → Completed.
- **Abort()**: `game::spell::StopCasting()` + `game::movement::StopMoving()`
- **Формат ротации** (внешний конфиг):
  ```
  priority | spell_id | condition
  1        | 47486    | target.hp_pct > 20 && !target.has_aura(47486)
  2        | 42845    | self.hp_pct < 30
  3        | 47488    | target.hp_pct <= 20
  4        | 6603     | always (auto-attack fallback)
  ```

#### FollowRoute
- **Параметры**: `vector<Vec3> waypoints`, `bool loop` (зацикленный маршрут)
- **Start()**: начинает движение к waypoints[0]
- **Tick()**: проверяет прибытие к текущей точке → переключается на следующую. Если все пройдены → Completed (или loop обратно к началу).
- **Abort()**: `StopCTM()` + `StopMoving()`

#### Interact
- **Параметры**: `GUID objectGuid`
- **Start()**: `game::movement::ClickToMoveInteract(guid, objectPos)`
- **Tick()**: проверяет дистанцию, завершает после взаимодействия
- **Abort()**: `StopCTM()`

#### Loot
- **Параметры**: нет (лутает текущую цель)
- **Start()**: `game::movement::ClickToMoveInteract(targetGuid, targetPos)`
- **Tick()**: ждёт лут-окно, завершает
- **Abort()**: `StopCTM()`

#### UseSpell
- **Параметры**: `uint32_t spellId` + опционально `GUID targetGuid`
- **Start()**: `game::spell::CastById(spellId)`
- **Tick()**: ждёт окончания каста → Completed. Если прервали — Failed.
- **Abort()**: `game::spell::StopCasting()`

#### Wait
- **Параметры**: `uint32_t durationMs`
- **Start()**: запоминает `GetTickCount64()`
- **Tick()**: проверяет прошло ли время → Completed
- **Abort()**: немедленно Cancelled

#### Sequence (композитный)
- **Параметры**: `vector<unique_ptr<ITool>> steps`
- **Start()**: запускает steps[0]
- **Tick()**: тикает текущий шаг. Если завершён → переходит к следующему. Если все шаги пройдены → Completed. Если любой Failed → Failed.
- **Abort()**: абортит текущий шаг, Cancelled.
- **Пример**: "Убить моба" = Sequence { MoveTo(mobPos), Attack(mobGuid), CombatRotation(rotation), Loot() }

---

## 3. ActionQueue (Очередь действий)

### 3.1 Структура

```cpp
class ActionQueue {
    std::deque<std::unique_ptr<ITool>> m_queue;

public:
    // --- Каждый кадр из EndScene ---
    void Tick();

    // --- Управление очередью ---
    void PushBack(std::unique_ptr<ITool> tool);     // Добавить в конец
    void Interrupt(std::unique_ptr<ITool> tool);     // Abort текущего + вставить в начало
    void InsertNext(std::unique_ptr<ITool> tool);    // Вставить на позицию 1 (после текущего)
    void Remove(size_t index);                       // Удалить элемент по индексу
    void Clear();                                    // Abort текущего + очистить всё

    // --- Чтение ---
    ITool* GetCurrent() const;                       // Первый элемент (Running или Pending)
    size_t Size() const;
    bool   IsEmpty() const;
    const std::deque<std::unique_ptr<ITool>>& GetAll() const;  // Для ImGui
};
```

### 3.2 Логика Tick

```cpp
void ActionQueue::Tick() {
    if (m_queue.empty()) return;

    auto* current = m_queue.front().get();

    // Запуск, если ещё Pending
    if (current->GetStatus() == ToolStatus::Pending)
        current->Start();

    // Тик, если Running
    if (current->GetStatus() == ToolStatus::Running)
        current->Tick();

    // Завершён? Убираем из очереди.
    auto st = current->GetStatus();
    if (st == ToolStatus::Completed || st == ToolStatus::Failed || st == ToolStatus::Cancelled)
        m_queue.pop_front();
    // Следующий элемент начнёт работу на следующем кадре.
}
```

### 3.3 Операции с очередью

**PushBack** — стандартная постановка задачи. Выполнится после всех текущих.

**Interrupt** — срочная задача. Текущий инструмент абортится и УДАЛЯЕТСЯ. Новый становится текущим.
```
До:    [CombatRotation, Loot, MoveTo(base)]
Interrupt(MoveTo(town)):
  → CombatRotation.Abort() → удалён
После: [MoveTo(town), Loot, MoveTo(base)]
```

**InsertNext** — поставить задачу СЛЕДУЮЩЕЙ, но НЕ прерывая текущую. Полезно для "после того как добьёшь, сделай вот это".
```
До:    [CombatRotation, MoveTo(base)]
InsertNext(Loot):
После: [CombatRotation, Loot, MoveTo(base)]
```

**Clear** — экстренная остановка. Abort текущего + удалить всё.

### 3.4 Решение: только Interrupt, БЕЗ паузы

Прерванный инструмент **не сохраняется** — он удаляется навсегда. Причины:

1. **Стухшее состояние**: пока мы отвлеклись, игровой мир изменился (моб умер, деспавнился, мы далеко). Возобновлять прерванный инструмент со старым состоянием опасно.
2. **Ответственность "мозга"**: внешний контроллер знает полную картину и решает, что делать после прерывания. Он может пушить новый инструмент с актуальными параметрами.
3. **Простота**: инструментам не нужна логика Pause/Resume/CanResume. Только Start/Tick/Abort.

---

## 4. IPC — Named Pipe + Protobuf

### 4.1 Транспорт

- **Named Pipe** (`\\.\pipe\wotlk_bot`)
- DLL создаёт pipe-сервер в отдельном потоке
- Внешний контроллер подключается как клиент
- Дуплексный: контроллер отправляет команды, DLL отвечает состоянием

### 4.2 Протокол

Каждое сообщение: `[4 байта длина LE][protobuf payload]`

Protobuf-схема определяет:

**Контроллер → DLL (команды)**:
- `PushBack(tool_definition)` — добавить инструмент в конец очереди
- `Interrupt(tool_definition)` — прервать текущий и запустить новый
- `InsertNext(tool_definition)` — вставить следующим
- `ClearQueue` — очистить очередь
- `RemoveAt(index)` — удалить конкретный элемент
- `GetState` — запросить текущее состояние (игрок, цель, очередь)

**DLL → Контроллер (ответы/события)**:
- `StateSnapshot` — полное состояние: позиция игрока, HP, цель, зона, очередь
- `ToolCompleted(tool_type, result)` — инструмент завершился
- `ToolFailed(tool_type, error)` — инструмент провалился
- `Error(message)` — ошибка выполнения команды

### 4.3 Потокобезопасность

```
Pipe Thread:
  1. Читает protobuf-сообщение из pipe
  2. Десериализует в Command
  3. Кладёт в thread-safe command_queue (std::queue + CRITICAL_SECTION)

EndScene (Main Thread):
  1. Забирает все команды из command_queue
  2. Применяет к ActionQueue (PushBack / Interrupt / Clear / ...)
  3. ActionQueue.Tick()
  4. Если клиент запросил GetState — собирает снапшот, кладёт в response_queue

Pipe Thread:
  1. Забирает ответы из response_queue
  2. Сериализует и отправляет через pipe
```

---

## 5. CombatRotation — внешняя база знаний

### 5.1 Формат ротации

Ротация — приоритетный список спеллов. Каждый элемент:

| Поле         | Тип     | Описание                                              |
|-------------|---------|-------------------------------------------------------|
| priority    | int     | Порядок проверки (меньше = выше приоритет)             |
| spell_id    | uint32  | ID спелла (WoW spell ID)                              |
| spell_name  | string  | Человекочитаемое имя (для UI/логов)                    |
| condition   | string  | Условие для каста (выражение)                          |

### 5.2 Условия (Condition Language)

Условия — простые выражения, вычисляемые каждый тик:

```
target.hp_pct > 20          // HP цели > 20%
self.hp_pct < 30            // Наш HP < 30%
!target.has_aura(47486)     // У цели нет ауры (дебаффа)
self.has_aura(12345)        // У нас есть бафф
!self.is_casting            // Не кастуем сейчас
self.power_pct > 40         // Мана/энергия/ярость > 40%
self.combo_points >= 5      // Комбо-очки
always                      // Безусловно (fallback)
```

### 5.3 Логика Tick

```
for each entry in rotation (sorted by priority):
    if spell is on cooldown → skip
    if !evaluate(entry.condition) → skip
    if !HasSpell(entry.spell_id) → skip
    CastById(entry.spell_id)
    break  // Только один каст за тик (GCD)
```

### 5.4 Источник ротации

Ротация передаётся как параметр инструмента CombatRotation — через Protobuf от контроллера или через ImGui-конфигуратор. DLL не хранит ротации — она получает их извне при каждом создании инструмента.

---

## 6. Sequence (композитный инструмент)

Sequence позволяет объединить несколько инструментов в один элемент очереди.

### 6.1 Поведение

- `Start()` → запускает первый шаг
- `Tick()` → тикает текущий шаг
  - Шаг Completed → переход к следующему шагу (Start)
  - Шаг Failed → весь Sequence → Failed
  - Все шаги пройдены → Sequence → Completed
- `Abort()` → абортит текущий шаг, весь Sequence → Cancelled
- `Describe()` → "Sequence [3/5]: CombatRotation"

### 6.2 Примеры

**"Убить моба":**
```
Sequence {
    MoveTo(mobPos),
    Attack(mobGuid),
    CombatRotation(rotation),
    Loot()
}
```

**"Фарм-маршрут":**
```
Sequence {
    FollowRoute(route, loop=false),
    // Контроллер добавляет CombatRotation через Interrupt при аггро
}
```

---

## 7. ImGui виджеты

> **Реализация**: два отдельных окна ImGui — "Tools" и "Queue" (в overlay.cpp).
> RenderParams() убран из ITool — UI параметров каждого инструмента рендерится через TabBar прямо в overlay.cpp.

### 7.1 Tool Panel (окно "Tools") ✅

- **TabBar** с вкладками: Wait, MoveTo, Attack, UseSpell, Loot, Interact, Kill & Loot
- Каждая вкладка: поля ввода параметров + кнопки PushBack / Interrupt
- "Kill & Loot" — создаёт SequenceTool {Attack, Loot}
- Реализовано полностью в `overlay.cpp` → `RenderToolsWidget()`

### 7.2 Queue Panel (окно "Queue") ✅

- Список всех инструментов в очереди с индикаторами статуса (►/✓/✗/⊘)
- Для Running-инструментов: прогресс-бар (Wait), дистанция (MoveTo), шаг N/M (Sequence)
- Кнопки [Remove] для каждого элемента, **"Clear All"** внизу
- Реализовано в `overlay.cpp` → `RenderQueueWidget()`

### 7.3 IPC Status (не реализовано)

- Индикатор подключения контроллера (Connected / Waiting)
- Счётчик принятых/выполненных команд
- Последняя команда (для дебага)

---

## 8. Файловая структура

```
wotlk/src/bot/
├── tool.h                     // ✅ ITool, ToolType, ToolStatus, ToolPtr
├── action_queue.h             // ✅ ActionQueue (singleton, deque)
├── action_queue.cpp           // ✅
│
├── tools/
│   ├── wait.h / .cpp          // ✅ WaitTool — GetProgress(), GetElapsedMs()
│   ├── move_to.h / .cpp       // ✅ MoveToTool — CTM + stuck detection
│   ├── attack.h / .cpp        // ✅ AttackTool — CTM Attack + re-attack
│   ├── use_spell.h / .cpp     // ✅ UseSpellTool — CD wait + cast
│   ├── loot.h / .cpp          // ✅ LootTool — CTM + Lua InteractUnit
│   ├── interact.h / .cpp      // ✅ InteractTool — CTM Interact + Lua
│   ├── sequence.h / .cpp      // ✅ SequenceTool — sub-tool vector
│   ├── combat_rotation.h / .cpp  // ❌ не реализовано
│   └── follow_route.h / .cpp    // ❌ не реализовано
│
├── ipc/                       // ❌ не реализовано
│   ├── pipe_server.h / .cpp
│   ├── command_queue.h / .cpp
│   └── proto/
│       └── bot.proto
│
└── (виджеты реализованы прямо в overlay.cpp, без отдельных файлов)
```

---

## 9. Интеграция в существующий код

### 9.1 overlay.cpp — тик + виджеты ✅ (реализовано)

```cpp
// В HookedEndScene, между NewFrame() и EndFrame():
bot::ActionQueue::Instance().Tick();  // Тикнуть текущий инструмент
RenderToolsWidget();                  // ImGui окно "Tools"
RenderQueueWidget();                  // ImGui окно "Queue"
```

> **Примечание**: виджеты и инклуды бот-инструментов добавлены прямо в overlay.cpp.
> Отдельная инициализация в dllmain.cpp не требуется — ActionQueue::Instance() работает как синглтон.

### 9.2 dllmain.cpp — инициализация IPC (не реализовано)

```cpp
// Будет добавлено в фазе 6:
bot::PipeServer::Initialize();
// В Shutdown:
bot::PipeServer::Shutdown();
```

### 9.3 Зависимости

- **Protobuf**: установить через vcpkg (`vcpkg install protobuf:x86-windows-static-md`)
- **ImGui**: уже интегрирован
- **MinHook**: уже интегрирован (для хуков, не для бота напрямую)
- **Game SDK**: уже реализован (`game::movement`, `game::spell`, `game::world`, и т.д.)

---

## 10. Порядок реализации

### ✅ Фаза 1 — Скелет (DONE)
- `tool.h` — ITool интерфейс, ToolType/ToolStatus enum, ToolPtr = unique_ptr<ITool>
- `action_queue.h/.cpp` — синглтон с deque, PushBack/Interrupt/InsertNext/Remove/Clear
- `wait.h/.cpp` — WaitTool с прогресс-баром
- ImGui встроен в overlay.cpp (первая версия — единый виджет)
- **Отличия от дизайна**: RenderParams() убран из ITool (UI рендерится в overlay.cpp напрямую)

### ✅ Фаза 2 — Базовые инструменты (DONE)
- `move_to.h/.cpp` — MoveToTool: ClickToMove + stuck detection (порог 1yd, таймаут 3с, 5 ретраев)
- `attack.h/.cpp` — AttackTool: SelectTarget + ClickToMoveAttack, ре-атака каждые 3с, завершается при смерти цели
- `use_spell.h/.cpp` — UseSpellTool: ожидание кулдауна + каст через Lua, грейс-период 200мс
- **Исправление**: DWORD заменён на uint32_t во всех хедерах (windows.h не включается в .h файлы)

### ✅ Фаза 3 — ImGui виджеты (DONE)
- Разделение на два отдельных ImGui-окна: **"Tools"** и **"Queue"**
- Tools: TabBar с вкладками (Wait, MoveTo, Attack, UseSpell, Loot, Interact, Kill & Loot)
- Queue: список с индикаторами статуса, прогресс-барами, кнопками Remove
- **Отличия от дизайна**: виджеты реализованы как функции в overlay.cpp, не в отдельных файлах widgets/

### ✅ Фаза 4 — Композитные инструменты (DONE)
- `sequence.h/.cpp` — SequenceTool: vector<ToolPtr> шагов, Describe() = "Sequence [N/M]: ..."
- `loot.h/.cpp` — LootTool: ClickToMove к трупу → Lua InteractUnit("target") при dist≤6yd → LootSlot() для всех предметов
- `interact.h/.cpp` — InteractTool: ClickToMoveInteract + Lua InteractUnit("target") при dist≤5yd
- Добавлен `ClickToMoveLoot()` в game::movement (CTM action 0x07)
- Вкладка "Kill & Loot" создаёт Sequence {Attack, Loot}
- **Ключевые фиксы**:
  - LootTool: сначала ClickToMove для навигации, затем Lua InteractUnit для надёжного открытия лут-окна
  - InteractTool: CTM Interact не работает на нулевой дистанции → fallback на Lua InteractUnit
  - Tab "Sequence" переименован в "Kill & Loot"

### ❌ Фаза 5 — CombatRotation (TODO)
- Парсер условий (condition language)
- Приоритетная ротация спеллов с проверкой кулдаунов/аур
- ImGui-конфигуратор ротации или загрузка из конфига

### ❌ Фаза 6 — IPC (TODO)
- Protobuf-схема (bot.proto)
- Named Pipe сервер (`\\.\pipe\wotlk_bot`)
- Thread-safe command_queue (CRITICAL_SECTION)
- Pipe thread ↔ EndScene main thread синхронизация

### ❌ Фаза 7 — Navigation System (TODO — разбита на подзадачи)

Полноценная система навигации с Detour навмешем, обходом враждебных NPC и гуманизацией движения.
Каждая подзадача задокументирована отдельно:

| Подфаза | Задача | Файл | Зависит от |
|---------|--------|------|------------|
| **7a** | Detour + mmtile loader | [`nav-detour-integration.md`](nav-detour-integration.md) | — |
| **7b** | FollowRouteTool (массив waypoints) | [`nav-follow-route.md`](nav-follow-route.md) | 7a (опц.) |
| **7c** | NavigateTool (авто-путь через Detour) | [`nav-navigate-tool.md`](nav-navigate-tool.md) | 7a, 7b |
| **7d** | Radar ImGui widget | [`radar-widget.md`](radar-widget.md) | — |
| **7e** | Hostile NPC avoidance + forced combat | [`nav-hostile-avoidance.md`](nav-hostile-avoidance.md) | 7c, 7d |
| **7f** | Movement humanization | [`nav-humanization.md`](nav-humanization.md) | 7c |
| **7g** | Road-preferring mmaps generator | [`nav-road-mmaps.md`](nav-road-mmaps.md) | 7a, 7c |

Ресерчи: `docs/reference/researches/` (Recast/Detour, movement packets, server-side detection).
