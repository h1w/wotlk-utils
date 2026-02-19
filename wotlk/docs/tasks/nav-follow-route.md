# Task: FollowRoute Tool

> **Status**: TODO
> **Created**: 2026-02-19
> **Phase**: 7b (Navigation)
> **Depends on**: nav-detour-integration (для валидации waypoints, опционально)
> **Blocks**: nav-navigate-tool

---

## Цель

Реализовать `FollowRouteTool` — ITool который проходит по заданному массиву waypoints через CTM. Это базовый "моторный" инструмент: получает готовый маршрут и идёт по нему. Не строит пути сам — это задача NavigateTool.

---

## Контекст

- ToolType::FollowRoute уже объявлен в `bot/tool.h`
- Существующий `MoveToTool` ходит к одной точке — FollowRoute проходит массив точек последовательно
- CTM (`game::movement::ClickToMove`) генерирует нормальные пакеты движения, неотличимые от ручного

---

## Что нужно сделать

### 1. FollowRouteTool

```cpp
// wotlk/src/bot/tools/follow_route.h

namespace bot {

class FollowRouteTool : public ITool {
public:
    // Основной конструктор — массив waypoints
    explicit FollowRouteTool(std::vector<game::Vec3> waypoints, bool loop = false);

    ToolType    GetType() const override   { return ToolType::FollowRoute; }
    const char* GetName() const override   { return "Follow Route"; }
    ToolStatus  GetStatus() const override { return m_status; }

    void Start() override;
    void Tick() override;
    void Abort() override;

    std::string Describe() const override;

    // Публичные геттеры для UI / Radar
    size_t GetCurrentWaypointIndex() const;
    size_t GetTotalWaypoints() const;
    const std::vector<game::Vec3>& GetWaypoints() const;
    float GetDistanceToCurrentWP() const;

private:
    std::vector<game::Vec3> m_waypoints;
    bool     m_loop = false;
    size_t   m_currentIndex = 0;
    float    m_arrivalThreshold = 2.0f;  // ярды — считаем прибывшим

    ToolStatus m_status = ToolStatus::Pending;

    // Stuck detection
    game::Vec3 m_lastPosition;
    uint64_t   m_lastMoveCheckTick = 0;
    uint32_t   m_stuckCount = 0;

    static constexpr float    kStuckThreshold  = 1.0f;  // yd — если прошёл меньше за интервал
    static constexpr uint32_t kStuckCheckMs    = 3000;   // проверка каждые 3с
    static constexpr uint32_t kMaxStuckRetries = 5;
    static constexpr uint32_t kTimeoutMs       = 300000; // 5 минут на весь маршрут

    uint64_t m_startTick = 0;

    void AdvanceToNext();
    void IssueCTMToCurrentWP();
};

} // namespace bot
```

### 2. Логика Tick

```
Start():
    1. Проверить что waypoints не пустой
    2. Вызвать CTM к waypoints[0]
    3. Запомнить позицию для stuck detection

Tick():
    1. Проверить таймаут (5 минут на весь маршрут)
    2. Проверить расстояние до текущего waypoint
    3. Если dist < arrivalThreshold:
       a. currentIndex++
       b. Если currentIndex >= waypoints.size():
          - Если loop → currentIndex = 0, CTM к первому
          - Иначе → Completed
       c. Иначе → CTM к следующему waypoint
    4. Stuck detection (каждые 3с):
       - Если прошли < 1yd за 3с → stuckCount++
       - Если stuckCount > 5 → Failed
       - Иначе → Jump() + повторный CTM
```

### 3. ImGui вкладка "Follow Route"

В окне Tools добавить вкладку:
- Поле ввода: количество waypoints (или кнопка "Record waypoints")
- Кнопка **"Add Current Position"** — добавляет текущую позицию игрока в массив
- Список добавленных точек с координатами
- Checkbox "Loop"
- Кнопки: Clear List, PushBack, Interrupt
- Отображение в Queue: "Follow Route [3/12] (45yd to next)"

### 4. Queue отображение

Для FollowRouteTool в RenderQueueList:
- Прогресс: `[currentIndex / totalWaypoints]`
- Дистанция до текущего waypoint
- Прогресс-бар: `currentIndex / totalWaypoints`

---

## Файловая структура

```
wotlk/src/bot/tools/
├── follow_route.h
└── follow_route.cpp
```

Модификация `overlay.cpp` — новая вкладка в TabBar Tools.

---

## Критерии готовности

- [ ] FollowRouteTool проходит массив waypoints через CTM
- [ ] Stuck detection работает (прыжок + retry при застревании)
- [ ] Loop mode — зацикленный маршрут
- [ ] ImGui: запись waypoints кнопкой "Add Current Position"
- [ ] Queue показывает прогресс [N/M] и дистанцию
- [ ] Таймаут 5 минут → Failed
