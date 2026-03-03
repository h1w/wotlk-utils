# Task: Navmesh Pathfinding for All Tools

> **Status**: DONE
> **Created**: 2026-02-19
> **Completed**: 2026-02-19
> **Phase**: 7c (Navigation)
> **Depends on**: nav-detour-integration, nav-follow-route
> **Blocks**: nav-hostile-avoidance, nav-humanization

---

## Цель

Интегрировать навмеш-навигацию во все инструменты движения (MoveToTool, AttackTool, LootTool, InteractTool) через переиспользуемый класс `NavHelper`.

---

## Контекст

- `Pathfinder::FindPath()` из nav-detour-integration возвращает массив waypoints
- `FollowRouteTool` уже умеет ходить по массиву waypoints

## Решение (вместо отдельного NavigateTool)

Вместо отдельного `NavigateTool` реализован **NavHelper** — переиспользуемый класс, который инкапсулирует логику pathfinding + следование по waypoints. Каждый инструмент встраивает NavHelper и делегирует ему навигацию. При недоступности навмеша инструменты fallback на прямой CTM (текущее поведение).

Это проще и естественнее: каждый инструмент сам управляет переключением между nav-подходом и прямым CTM в зависимости от расстояния до цели.

---

## Реализация

### NavHelper (`wotlk/src/bot/nav_helper.h` + `.cpp`)

Переиспользуемый класс, инкапсулирующий pathfinding + waypoint following:

```cpp
class NavHelper {
public:
    enum class Status : uint8_t { Idle, Moving, Arrived, Failed };

    bool StartNavTo(const game::Vec3& target);  // FindPath + start following
    Status Tick();                                // tick waypoint following
    void Stop();                                  // stop + StopCTM

    Status GetStatus() const;
    bool   IsActive() const;  // Status == Moving
};
```

- Stuck detection: 1yd threshold, 3s check interval, jump + re-CTM on stuck, 5 max retries
- On Arrived: does NOT call StopCTM (lets calling tool issue its own CTM)
- Skips first waypoint if player is already close to it

### Интеграция в инструменты

| Инструмент | kNavSwitchRange | Поведение |
|---|---|---|
| **MoveToTool** | — (навигация до цели) | Nav → Completed. Fallback: direct CTM |
| **AttackTool** | 15yd | Nav approach → switch to ClickToMoveAttack |
| **LootTool** | 10yd | Nav approach → switch to ClickToMoveInteract |
| **InteractTool** | 10yd | Nav approach → switch to ClickToMoveInteract |

Каждый инструмент:
1. В `Start()`: пробует `m_nav.StartNavTo()`. Если false → fallback на прямой CTM
2. В `Tick()`: если nav mode → тикает NavHelper, проверяет расстояние для switch/arrived/failed
3. В `Abort()`: вызывает `m_nav.Stop()` если активен

### AttackTool — рефакторинг устойчивости

Помимо навигации, AttackTool был переработан для устойчивости к:
- **Ручной деселекции цели** (LMB по пустому месту) — re-select каждый тик
- **Отмена CTM поворотом камеры** (RMB) — progress check каждую 1с детектит остановку

Два режима прямой атаки:
- **В ближнем бою** (≤8yd): только Lua `AttackTarget()` (не зависит от CTM)
- **Вне ближнего боя**: progress check + periodic CTM refresh

### ImGui

- Вкладка "MoveTo" удалена — заменена вкладкой "Navigate" (навигация через навмеш)
- Кнопка "MoveTo + Kill + Loot" обновлена: `MoveToTool(pos)` без параметра `arrivalDist`

---

## Файловая структура

```
wotlk/src/bot/
├── nav_helper.h / .cpp     // NavHelper — pathfinding + waypoint following
├── tools/
│   ├── move_to.h / .cpp    // + NavHelper, убран arrivalDist
│   ├── attack.h / .cpp     // + NavHelper + resilience rework
│   ├── loot.h / .cpp       // + NavHelper
│   └── interact.h / .cpp   // + NavHelper
```

---

## Критерии готовности

- [x] NavHelper строит путь через Detour и проходит его с stuck detection
- [x] MoveToTool использует NavHelper, fallback на прямой CTM
- [x] AttackTool: nav approach + switch на прямую атаку при ≤15yd
- [x] LootTool: nav approach + switch на interact при ≤10yd
- [x] InteractTool: nav approach + switch на interact при ≤10yd
- [x] AttackTool устойчив к деселекции цели и отмене CTM
- [x] ImGui: вкладка Navigate, удалена вкладка MoveTo
