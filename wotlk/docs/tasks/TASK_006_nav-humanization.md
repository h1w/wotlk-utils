# Task: Movement Humanization

> **Status**: TODO
> **Created**: 2026-02-19
> **Phase**: 7f (Navigation)
> **Depends on**: nav-navigate-tool
> **Blocks**: nothing

---

## Цель

Добавить слой гуманизации поверх навигации — чтобы движение бота было неотличимо от движения реального игрока. Сглаживание пути, микро-отклонения, случайные прыжки, микро-паузы, управление маунтом.

---

## Контекст из ресерча

- **CTM безопасен** — генерирует нормальные движественные пакеты
- **Серверная валидация минимальна** — только проверки скорости/флая/телепорта
- **Основная угроза — живые ГМы** — они замечают идеально прямые линии, фиксированные интервалы прыжков, отсутствие поворотов facing
- **Ключевые техники**: Catmull-Rom сплайны, Perlin noise (0.1–0.5yd), Poisson-прыжки (10–30с), микро-паузы, неточные остановки

---

## Что нужно сделать

### 1. PathSmoother — сглаживание пути

Превращает ломаную линию waypoints в плавную кривую.

```cpp
// wotlk/src/navigation/path_smoother.h

namespace nav {

class PathSmoother {
public:
    // Catmull-Rom сплайн интерполяция
    // Вход: waypoints от Detour (ломаная)
    // Выход: более плотный массив точек (плавная кривая)
    static std::vector<game::Vec3> SmoothCatmullRom(
        const std::vector<game::Vec3>& path,
        int samplesPerSegment = 4  // точек между каждой парой waypoints
    );

    // Bezier сглаживание углов
    // Для каждого поворота > threshold градусов — скругляет
    static std::vector<game::Vec3> SmoothCorners(
        const std::vector<game::Vec3>& path,
        float cornerAngleThreshold = 30.0f,  // градусов
        float cornerRadius = 2.0f            // ярдов
    );

private:
    // Catmull-Rom: P(t) = 0.5 * ((2*P1) + (-P0+P2)*t + (2*P0-5*P1+4*P2-P3)*t² + (-P0+3*P1-3*P2+P3)*t³)
    static game::Vec3 CatmullRom(
        const game::Vec3& p0, const game::Vec3& p1,
        const game::Vec3& p2, const game::Vec3& p3,
        float t);
};

} // namespace nav
```

### 2. PathHumanizer — микро-отклонения

Добавляет шум к уже сглаженному пути.

```cpp
// wotlk/src/navigation/path_humanizer.h

namespace nav {

class PathHumanizer {
public:
    // Perlin noise displacement — смещение каждой точки перпендикулярно пути
    // amplitude: 0.1–0.5 ярдов (мало — чтобы не выйти за навмеш)
    static void ApplyPerlinNoise(
        std::vector<game::Vec3>& path,
        float amplitude = 0.3f,
        float frequency = 0.1f  // частота шума (ниже = плавнее)
    );

    // Случайное смещение каждого waypoint
    // sigma: стандартное отклонение в ярдах (Gaussian)
    static void ApplyGaussianJitter(
        std::vector<game::Vec3>& path,
        float sigma = 0.5f
    );

    // Неточная остановка: смещение финальной точки на ±offset ярдов
    static void ApplyStopImprecision(
        std::vector<game::Vec3>& path,
        float maxOffset = 1.5f
    );

private:
    // Простая реализация Perlin noise (1D)
    static float PerlinNoise1D(float x);
};

} // namespace nav
```

### 3. JumpScheduler — случайные прыжки

```cpp
// wotlk/src/bot/jump_scheduler.h

namespace bot {

class JumpScheduler {
public:
    static JumpScheduler& Instance();

    // Вызывать каждый тик из NavigateTool / FollowRouteTool
    // Возвращает true если пора прыгнуть
    bool ShouldJump();

    // Настройка
    void SetEnabled(bool enabled);
    void SetMeanIntervalSec(float seconds); // default = 20.0

private:
    bool     m_enabled = true;
    float    m_meanInterval = 20.0f;  // Poisson: λ = 1/20с
    uint64_t m_lastJumpTick = 0;
    uint64_t m_nextJumpTick = 0;      // рассчитывается заранее

    void ScheduleNextJump();

    // Poisson: время до следующего события = -ln(U) / λ
    // где U ~ Uniform(0,1), λ = 1/meanInterval
    float SamplePoissonInterval();
};

} // namespace bot
```

**Распределение прыжков**: NOT фиксированный интервал. Poisson-процесс создаёт натуральную кластеризацию — иногда 3 прыжка за 10 секунд, иногда тишина на минуту.

### 4. MountManager — автоматический маунт

```cpp
// wotlk/src/bot/mount_manager.h

namespace bot {

class MountManager {
public:
    static MountManager& Instance();

    // Вызывать при старте навигации — решает маунтить или нет
    bool ShouldMount(float totalPathDistance);

    // Сесть на маунт (через Lua)
    bool Mount();

    // Спешиться
    bool Dismount();

    // Проверить что уже на маунте
    bool IsMounted() const;

    // Настройка
    void SetMountSpellId(uint32_t spellId);
    void SetMinMountDistance(float yards);  // default = 50yd

private:
    uint32_t m_mountSpellId = 0;        // ID спелла маунта
    float    m_minMountDistance = 50.0f; // не маунтить на короткие дистанции

    // Задержка перед маунтом (1-3 сек) — люди не маунтятся мгновенно
    float    m_mountDelay = 0.0f;
};

} // namespace bot
```

Логика:
- Маунт если дистанция > 50yd И нет врагов в 30yd
- Задержка 1-3с перед маунтом (как человек)
- Спешиться автоматически при входе в бой / при прибытии
- Не маунтить в помещениях / подземельях (проверка через Lua `IsMounted()`, `IsOutdoors()`)

### 5. FacingUpdater — обновление ориентации

```cpp
// wotlk/src/bot/facing_updater.h

namespace bot {

class FacingUpdater {
public:
    static FacingUpdater& Instance();

    // Вызывать из NavigateTool/FollowRoute при смене waypoint
    // Плавно поворачивает facing к следующей точке
    void UpdateFacing(const game::Vec3& nextWaypoint);

    // Периодический "взгляд по сторонам" — имитация камеры
    void RandomLookAround();

private:
    uint64_t m_lastLookAroundTick = 0;

    // Плавный поворот (не мгновенный snap)
    // interpolationTime: 200-400ms
    void SmoothSetFacing(float targetAngle, float interpolationTimeMs = 300.0f);
};

} // namespace bot
```

**Зачем**: CTM автоматически поворачивает персонажа, но facing не обновляется в movement packets при длинных прямых сегментах. Периодический `SetFacing` + случайные мелкие повороты ("смотрит по сторонам") — имитация реального игрока.

### 6. MicroPause — случайные микро-паузы

```cpp
// Встраивается в NavigateTool/FollowRouteTool

// Каждые 30-120 секунд — пауза 50-200мс
// Имитирует: человек отвлёкся, поправил руку на мышке, посмотрел в чат
class MicroPauseScheduler {
public:
    bool ShouldPause();
    uint32_t GetPauseDurationMs(); // 50-200мс
private:
    float m_meanIntervalSec = 60.0f; // в среднем раз в минуту
};
```

### 7. Пайплайн гуманизации

Порядок применения к пути:

```
1. Detour findStraightPath() → raw waypoints (ломаная)
2. PathSmoother::SmoothCatmullRom() → плавная кривая
3. PathSmoother::SmoothCorners() → скруглённые повороты
4. PathHumanizer::ApplyPerlinNoise() → микро-отклонения
5. PathHumanizer::ApplyStopImprecision() → неточная остановка
```

Во время движения (каждый тик):
```
6. JumpScheduler::ShouldJump() → периодический прыжок
7. MicroPauseScheduler::ShouldPause() → пауза
8. FacingUpdater::UpdateFacing() → плавный поворот при смене waypoint
9. FacingUpdater::RandomLookAround() → случайный взгляд по сторонам
```

### 8. ImGui контролы

В окне Tools (или отдельное окно "Navigation Settings"):
- **Checkbox**: Enable Humanization (master toggle)
- **Slider**: Noise amplitude (0.0 – 1.0 yd)
- **Slider**: Jump interval (5 – 60 sec)
- **Checkbox**: Auto-mount
- **Input**: Mount spell ID
- **Slider**: Min mount distance (10 – 200 yd)
- **Checkbox**: Random look-around

---

## Файловая структура

```
wotlk/src/navigation/
├── path_smoother.h / .cpp      // Catmull-Rom, Bezier corners
├── path_humanizer.h / .cpp     // Perlin noise, Gaussian jitter, stop imprecision

wotlk/src/bot/
├── jump_scheduler.h / .cpp     // Poisson-distributed jumps
├── mount_manager.h / .cpp      // Auto-mount logic
├── facing_updater.h / .cpp     // Smooth facing + random look-around
├── micro_pause.h / .cpp        // Random micro-pauses
```

---

## Критерии готовности

- [ ] PathSmoother: Catmull-Rom сплайн превращает ломаную в плавную кривую
- [ ] PathHumanizer: Perlin noise добавляет микро-отклонения (0.1–0.5yd)
- [ ] Неточная остановка (±1.5yd от целевой точки)
- [ ] JumpScheduler: Poisson-распределение, среднее ~20с
- [ ] MountManager: авто-маунт на дистанции >50yd, задержка 1-3с
- [ ] FacingUpdater: плавный поворот + случайные взгляды
- [ ] MicroPause: паузы 50-200мс каждые 30-120с
- [ ] ImGui контролы для настройки всех параметров
- [ ] Визуально: бот ходит как реальный игрок (проверка наблюдением)
