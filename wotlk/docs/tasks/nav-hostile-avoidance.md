# Task: Hostile NPC Avoidance + Forced Combat

> **Status**: TODO
> **Created**: 2026-02-19
> **Phase**: 7e (Navigation)
> **Depends on**: nav-navigate-tool, radar-widget (для визуализации)
> **Blocks**: nothing

---

## Цель

Добавить в навигацию обход враждебных NPC. Если обход невозможен — автоматически вступить в бой с NPC, блокирующим путь, добить, залутать, и продолжить маршрут.

---

## Контекст

- NavigateTool строит путь через Detour и ходит по нему
- ObjectManager даёт позиции всех NPC
- UnitReaction определяет враждебность
- AttackTool и LootTool уже реализованы (Phase 4)
- Radar widget отображает aggro-зоны (отдельная задача)
- Из ресерча: aggro radius = `20 + (mobLevel - playerLevel)`, clamped [5, 45], + safety margin

---

## Что нужно сделать

### 1. Threat Scanner

```cpp
// wotlk/src/bot/threat_scanner.h

namespace bot {

struct ThreatInfo {
    game::GUID  guid;
    game::Vec3  position;
    std::string name;
    int         level;
    float       aggroRadius;    // с safety margin
    float       distToPlayer;
    bool        isOnPath;       // пересекает текущий путь
};

class ThreatScanner {
public:
    static ThreatScanner& Instance();

    // Обновить список угроз (вызывать из EndScene, ~200мс)
    void Update();

    // Все враждебные NPC в пределах видимости
    const std::vector<ThreatInfo>& GetThreats() const;

    // Проверить пересечение пути с aggro-зонами
    // Возвращает индекс первого waypoint в опасной зоне, или -1
    int FindFirstThreatOnPath(const std::vector<game::Vec3>& path) const;

    // Получить NPC, блокирующие путь
    std::vector<ThreatInfo> GetBlockingThreats(const std::vector<game::Vec3>& path) const;

    // Найти самого слабого NPC из блокирующих
    const ThreatInfo* GetWeakestBlockingThreat(const std::vector<game::Vec3>& path) const;

private:
    std::vector<ThreatInfo> m_threats;
    uint64_t m_lastUpdateTick = 0;
    static constexpr uint32_t kUpdateIntervalMs = 200;
    static constexpr float kScanRadius = 80.0f; // ярдов вокруг игрока

    float CalcAggroRadius(int creatureLevel, int playerLevel) const;
    bool SegmentIntersectsCircle(
        const game::Vec3& segA, const game::Vec3& segB,
        const game::Vec3& center, float radius) const;
};

} // namespace bot
```

### 2. Path Post-Processing (обход)

Когда NavigateTool обнаруживает угрозу на пути:

```
1. Получить блокирующие NPC
2. Для каждого NPC:
   a. Вычислить обходную точку:
      - Перпендикуляр к сегменту пути, смещённый на aggroRadius + margin
      - Два варианта: обход слева или справа
   b. Проверить обходную точку:
      - Она на навмеше? (findNearestPoly)
      - TraceLine: нет стены между текущей точкой → обходной → следующей?
   c. Выбрать лучший вариант (ближайший / без других угроз)
3. Вставить обходные waypoints в путь
4. Пересчитать путь через Detour если обходная точка далеко
```

```cpp
namespace bot {

struct AvoidanceResult {
    std::vector<game::Vec3> modifiedPath;
    bool avoidancePossible;           // удалось обойти всех
    std::vector<ThreatInfo> unavoidable; // кого не обойти
};

AvoidanceResult AvoidThreats(
    const std::vector<game::Vec3>& originalPath,
    const std::vector<ThreatInfo>& threats);

} // namespace bot
```

### 3. Forced Combat (принудительный бой)

Если `AvoidanceResult::avoidancePossible == false`:

```
1. Выбрать самого слабого NPC из unavoidable
   - Критерии: lowest level, lowest HP, ближайший к пути
2. NavigateTool переходит в режим "forced combat":
   a. InsertNext в ActionQueue:
      Sequence {
          Attack(weakestNPC.guid),
          Loot(weakestNPC.guid),   // если мертвый — залутать
      }
   b. NavigateTool приостанавливает себя (ждёт пока Sequence завершится)
   c. После завершения Sequence → пересчитать путь
   d. Повторить проверку (может открылся проход)
3. Если боёв слишком много (>5 подряд) → лог предупреждение
```

**Важно**: NavigateTool сам не дерётся — он создаёт Attack+Loot через ActionQueue и ждёт. Это разделение ответственности: Navigate = навигация, Attack = бой.

### 4. Интеграция в NavigateTool

Добавить в `NavigateTool::Tick()`:

```cpp
void NavigateTool::Tick() {
    // ... existing logic ...

    // Каждые 2с — проверить угрозы на пути
    if (now - m_lastThreatCheckTick > kThreatCheckMs) {
        m_lastThreatCheckTick = now;

        auto& scanner = ThreatScanner::Instance();
        auto threats = scanner.GetBlockingThreats(m_path);

        if (!threats.empty()) {
            auto result = AvoidThreats(m_path, threats);

            if (result.avoidancePossible) {
                // Обходим: заменяем путь
                m_path = std::move(result.modifiedPath);
                m_currentIndex = 0;
                IssueCTMToCurrentWP();
            } else {
                // Не обойти: бой
                auto* weakest = scanner.GetWeakestBlockingThreat(m_path);
                if (weakest) {
                    EnterForcedCombat(weakest->guid);
                }
            }
        }
    }
}
```

### 5. TraceLine для валидации обхода

Используем `offsets::fn::TraceLine` (0x007A3B70) для проверки что обходной путь не проходит через стену:

```cpp
// game::world namespace (добавить)
bool HasLineOfSight(const Vec3& from, const Vec3& to);
```

Флаги: `HitTestWMO | HitTestGround | HitTestBoundingModels` (0x100111).

### 6. Визуализация в Radar

ThreatScanner данные используются Radar виджетом для отрисовки:
- Красные круги aggro-зон
- Подсветка NPC на пути (мигающий красный)
- Обходные waypoints (жёлтые точки)
- Текст "FORCED COMBAT" когда бот вступает в бой

---

## Файловая структура

```
wotlk/src/bot/
├── threat_scanner.h / .cpp    // ThreatScanner, ThreatInfo
├── path_avoidance.h / .cpp    // AvoidThreats(), обходная логика
```

Модификация: `navigate.cpp` (добавить threat checking + forced combat).

---

## Критерии готовности

- [ ] ThreatScanner сканирует hostile NPC каждые 200мс
- [ ] Aggro radius рассчитывается по формуле TrinityCore
- [ ] Пересечение пути с aggro-зонами определяется корректно
- [ ] Обходные waypoints генерируются и валидируются через TraceLine
- [ ] Если обход невозможен → создаётся Attack+Loot Sequence
- [ ] После боя путь пересчитывается
- [ ] Radar отображает aggro-зоны и угрозы на пути
- [ ] TraceLine (`HasLineOfSight`) работает для валидации обхода
