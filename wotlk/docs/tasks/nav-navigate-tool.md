# Task: Navigate Tool (Auto-Pathfinding)

> **Status**: TODO
> **Created**: 2026-02-19
> **Phase**: 7c (Navigation)
> **Depends on**: nav-detour-integration, nav-follow-route
> **Blocks**: nav-hostile-avoidance, nav-humanization

---

## Цель

Реализовать `NavigateTool` — ITool который принимает целевую точку (x, y, z), автоматически строит путь через Detour навмеш, и проходит его. Это "умная" навигация поверх FollowRouteTool.

---

## Контекст

- `Pathfinder::FindPath()` из nav-detour-integration возвращает массив waypoints
- `FollowRouteTool` уже умеет ходить по массиву waypoints
- NavigateTool = Pathfinder + FollowRoute + динамический пересчёт пути

---

## Что нужно сделать

### 1. NavigateTool

Добавить `ToolType::Navigate` в `tool.h`.

```cpp
// wotlk/src/bot/tools/navigate.h

namespace bot {

class NavigateTool : public ITool {
public:
    explicit NavigateTool(game::Vec3 destination);

    ToolType    GetType() const override   { return ToolType::Navigate; }
    const char* GetName() const override   { return "Navigate"; }
    ToolStatus  GetStatus() const override { return m_status; }

    void Start() override;
    void Tick() override;
    void Abort() override;

    std::string Describe() const override;

    // Публичные геттеры для Radar
    const game::Vec3& GetDestination() const;
    const std::vector<game::Vec3>& GetPath() const;
    size_t GetCurrentWaypointIndex() const;

private:
    game::Vec3 m_destination;
    ToolStatus m_status = ToolStatus::Pending;

    // Внутренний маршрут (результат findPath)
    std::vector<game::Vec3> m_path;
    size_t   m_currentIndex = 0;
    float    m_arrivalThreshold = 2.5f;

    // Пересчёт пути
    uint64_t m_lastRecalcTick = 0;
    static constexpr uint32_t kRecalcIntervalMs = 5000; // пересчёт каждые 5с (если надо)
    static constexpr float    kRecalcDistThresh = 10.0f; // пересчитать если отклонились >10yd

    // Stuck detection (аналогично FollowRoute)
    game::Vec3 m_lastPosition;
    uint64_t   m_lastStuckCheckTick = 0;
    uint32_t   m_stuckCount = 0;
    static constexpr float    kStuckThreshold  = 1.0f;
    static constexpr uint32_t kStuckCheckMs    = 3000;
    static constexpr uint32_t kMaxStuckRetries = 5;

    // Таймаут
    uint64_t m_startTick = 0;
    static constexpr uint32_t kTimeoutMs = 600000; // 10 минут

    bool CalculatePath();
    void IssueCTMToCurrentWP();
    void AdvanceWaypoint();
    bool ShouldRecalcPath();
};

} // namespace bot
```

### 2. Логика

```
Start():
    1. Проверить что NavMesh загружен и готов
    2. Вызвать Pathfinder::FindPath(playerPos, destination)
    3. Если путь не найден → Failed
    4. Если путь partial → предупреждение в лог, идём сколько можем
    5. CTM к первому waypoint

Tick():
    1. Таймаут → Failed
    2. Проверить расстояние до destination (а не до waypoint):
       - Если dist < arrivalThreshold → Completed
    3. Проверить расстояние до текущего waypoint:
       - Если dist < threshold → AdvanceWaypoint()
    4. Периодический пересчёт пути (каждые 5с):
       - Если отклонились от пути > kRecalcDistThresh → пересчитать
       - Это нужно если игрока сдвинули (knockback, агро, и т.д.)
    5. Stuck detection → прыжок + пересчёт пути

ShouldRecalcPath():
    - Прошло > 5с с последнего расчёта
    - Расстояние от текущей позиции до ближайшей точки на пути > 10yd
    - Текущий mapId изменился (телепорт)
```

### 3. Взаимодействие с NavMesh

```cpp
bool NavigateTool::CalculatePath() {
    auto player = game::GetLocalPlayer();
    if (!player) return false;

    game::Vec3 startPos = player->GetPosition();
    nav::PathResult result = nav::Pathfinder::Instance().FindPath(startPos, m_destination);

    if (!result.success && result.waypoints.empty())
        return false;

    m_path = std::move(result.waypoints);
    m_currentIndex = 0;
    return true;
}
```

### 4. ImGui вкладка "Navigate"

В окне Tools добавить вкладку:
- 3 float-поля (X, Y, Z) для целевой точки
- Кнопка **"Use Target Position"** — координаты текущей цели
- Кнопка **"Use Current Position"** — текущие координаты игрока (для тестов)
- Текст: расстояние до цели
- Текст: статус NavMesh (загружен / не загружен / сколько тайлов)
- Кнопки: PushBack, Interrupt

### 5. Отличие от FollowRoute

| | FollowRoute | Navigate |
|---|---|---|
| Вход | Массив Vec3 (готовый) | Одна точка-цель |
| Путь | Задан заранее | Строится через Detour |
| Пересчёт | Нет | Да, каждые 5с при отклонении |
| Зависит от NavMesh | Нет | Да |
| Использование | Записанные маршруты, тесты | Основная навигация |

---

## Файловая структура

```
wotlk/src/bot/tools/
├── navigate.h
└── navigate.cpp
```

Модификация: `tool.h` (добавить ToolType::Navigate), `overlay.cpp` (новая вкладка).

---

## Критерии готовности

- [ ] NavigateTool строит путь через Detour и проходит его
- [ ] Пересчёт пути при отклонении > 10yd
- [ ] Stuck detection + прыжок + пересчёт
- [ ] Partial path: идём сколько можем, потом Failed
- [ ] ImGui вкладка с вводом координат
- [ ] Queue показывает дистанцию до цели и прогресс пути
