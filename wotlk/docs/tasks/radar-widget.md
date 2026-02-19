# Task: Radar ImGui Widget

> **Status**: TODO
> **Created**: 2026-02-19
> **Phase**: 7d (Navigation / UI)
> **Depends on**: nothing (использует только ObjectManager и game SDK)
> **Blocks**: nothing (но полезен для отладки всех nav-задач)

---

## Цель

Создать ImGui виджет "Radar" — top-down 2D карта окружения с отображением игрока, NPC, враждебных мобов, пути навигации, препятствий и aggro-зон. Основная цель — визуальная отладка навигации и мониторинг работы бота.

---

## Контекст

- ImGui уже интегрирован (overlay.cpp), есть окна "Tools" и "Queue"
- ObjectManager (`game::objmgr::EnumObjects`) позволяет перечислить все объекты
- `game::Unit` / `game::WowObject` дают позицию, тип, фракцию
- `offsets::fn::UnitReaction` = 0x007251C0 — определяет враждебность
- ImGui DrawList API: `AddCircleFilled`, `AddLine`, `AddTriangleFilled`, `AddPolyline`, `AddText`

---

## Что нужно сделать

### 1. Отдельное ImGui окно "Radar"

```cpp
void RenderRadarWidget() {
    ImGui::Begin("Radar", nullptr, ImGuiWindowFlags_NoScrollbar);
    // ... рендер радара ...
    ImGui::End();
}
```

Вызывается из `HookedEndScene()` рядом с `RenderToolsWidget()` и `RenderQueueWidget()`.

### 2. Рендеринг радара

#### 2.1 Холст

- Квадратная область ImGui (300x300 по умолчанию, ресайзится с окном)
- Игрок всегда в центре
- Масштаб: настраиваемый слайдером (10yd – 200yd видимый радиус)
- Режим ориентации (toggle):
  - **North-Up**: север всегда вверху (как на обычной карте)
  - **Player-Facing-Up**: направление игрока вверху (как в автомобильном навигаторе)
- Фон: тёмно-серый с концентрическими кругами (каждые 10/25/50yd в зависимости от масштаба)

#### 2.2 Координатная трансформация

```cpp
// Мировые координаты → пиксели на радаре
ImVec2 WorldToRadar(const game::Vec3& worldPos) {
    float dx = worldPos.x - playerPos.x;
    float dy = worldPos.y - playerPos.y;

    // Если Player-Facing-Up → повернуть на -playerFacing
    if (playerFacingUp) {
        float cos_f = cosf(-playerFacing);
        float sin_f = sinf(-playerFacing);
        float rx = dx * cos_f - dy * sin_f;
        float ry = dx * sin_f + dy * cos_f;
        dx = rx; dy = ry;
    }

    float scale = radarRadius / visibleRange; // пиксели на ярд
    return ImVec2(
        radarCenter.x + dy * scale,  // Y мира → X экрана (WoW координаты)
        radarCenter.y - dx * scale   // X мира → Y экрана (инвертировано)
    );
}
```

**Важно**: WoW использует нестандартную систему координат (X = юг, Y = запад). Нужно аккуратно маппить на экранные координаты.

#### 2.3 Элементы радара

| Элемент | Фигура | Цвет | Описание |
|---------|--------|------|----------|
| **Игрок** | Треугольник (стрелка) | Белый | Направлен по facing |
| **Враждебный NPC** | Круг | Красный | Radius пропорционален уровню |
| **Aggro-зона** | Круг (полупрозрачный) | Красный (alpha=0.15) | Радиус = aggroRadius + 3yd margin |
| **Нейтральный NPC** | Круг | Жёлтый | Маленький |
| **Дружественный NPC** | Круг | Зелёный | Маленький |
| **Другой игрок** | Ромб | Синий | |
| **Путь навигации** | Полилиния | Зелёный (яркий) | От игрока до финальной точки |
| **Текущий waypoint** | Круг (filled) | Зелёный | Точка к которой сейчас идём |
| **Целевая точка** | Звезда / X | Золотой | Конечная цель NavigateTool |
| **Мёртвый NPC** | Круг (outline) | Серый | Не закрашенный |
| **GameObject** | Квадрат | Оранжевый | Руда, травы, сундуки |
| **Линии расстояний** | Концентрические круги | Серый (alpha=0.3) | Каждые N ярдов |

#### 2.4 Информация на радаре

- Текст в углу: текущий масштаб (например "50yd")
- Текст в углу: количество hostile / friendly / players в радиусе
- Стрелка "N" (север) — если Player-Facing-Up mode
- При наведении мышки на точку: тултип с именем NPC, уровнем, расстоянием

### 3. Данные для радара

#### Сбор данных (каждые 100-500мс, НЕ каждый фрейм)

```cpp
struct RadarEntry {
    game::Vec3    position;
    game::GUID    guid;
    std::string   name;
    int           level;
    int           health;       // текущий HP
    int           maxHealth;
    game::ObjectType objType;   // Unit, Player, GameObject
    game::UnitReaction reaction;
    bool          isDead;
    bool          isInCombat;
    float         aggroRadius;  // только для hostile NPC
};

class RadarData {
public:
    void Update();  // Вызвать из EndScene каждые ~200мс
    const std::vector<RadarEntry>& GetEntries() const;

private:
    std::vector<RadarEntry> m_entries;
    uint64_t m_lastUpdateTick = 0;
    static constexpr uint32_t kUpdateIntervalMs = 200;
};
```

#### Формула aggro radius (из TrinityCore)

```cpp
float CalcAggroRadius(int creatureLevel, int playerLevel) {
    float radius = 20.0f + (float)(creatureLevel - playerLevel);
    if (radius < 5.0f)  radius = 5.0f;
    if (radius > 45.0f) radius = 45.0f;
    return radius + 3.0f; // safety margin
}
```

### 4. Интеграция с путём навигации

Радар должен уметь рисовать путь от текущих NavigateTool / FollowRouteTool:

```cpp
// Проверить текущий инструмент в ActionQueue
ITool* current = bot::ActionQueue::Instance().GetCurrent();
if (current) {
    if (current->GetType() == ToolType::Navigate) {
        auto* nav = static_cast<NavigateTool*>(current);
        DrawPath(nav->GetPath(), nav->GetCurrentWaypointIndex());
        DrawDestination(nav->GetDestination());
    }
    if (current->GetType() == ToolType::FollowRoute) {
        auto* fr = static_cast<FollowRouteTool*>(current);
        DrawPath(fr->GetWaypoints(), fr->GetCurrentWaypointIndex());
    }
}
```

### 5. ImGui контролы

Внизу или сбоку радара:
- **Slider**: Visible Range (10–200 yd)
- **Toggle**: North-Up / Player-Facing-Up
- **Checkboxes**: Show Hostiles, Show Friendlies, Show Players, Show GameObjects, Show Path, Show Aggro Zones, Show Dead
- **Checkbox**: Show Navmesh (wireframe) — опционально, для отладки

---

## Файловая структура

```
wotlk/src/bot/
├── radar.h             // RadarData + RadarEntry struct
├── radar.cpp           // Update() — сбор данных из ObjectManager
```

Рендеринг — в `overlay.cpp` → `RenderRadarWidget()`.

---

## Существующие ресурсы

- `game::objmgr::EnumObjects()` — перечисление всех объектов
- `game::Unit` — GetPosition(), GetUnitName(), GetLevel(), GetHealth(), etc.
- `offsets::fn::UnitReaction` — определение враждебности
- `offsets::fields::UNIT_LEVEL`, `UNIT_HEALTH`, `UNIT_FLAGS`
- ImGui DrawList API — `GetWindowDrawList()`, все примитивы рисования

---

## Критерии готовности

- [ ] Отдельное ImGui окно "Radar" с top-down видом
- [ ] Игрок в центре как треугольник-стрелка
- [ ] Враждебные NPC — красные точки с aggro-зонами
- [ ] Дружественные/нейтральные NPC, игроки — цветные точки
- [ ] Путь навигации — зелёная полилиния (если активен Navigate/FollowRoute)
- [ ] Масштаб настраивается слайдером
- [ ] Режимы North-Up / Player-Facing-Up
- [ ] Тултипы с инфо при наведении
- [ ] Обновление данных каждые ~200мс (не тормозит рендер)
