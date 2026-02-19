# Task: Road-Preferring MMAPs Generator

> **Status**: TODO (на подумать)
> **Created**: 2026-02-19
> **Phase**: 7g (Navigation — последняя)
> **Depends on**: nav-detour-integration, nav-navigate-tool
> **Blocks**: nothing

---

## Цель

Создать модифицированную версию TrinityCore mmaps_generator, которая помечает дорожные полигоны навмеша специальным area type. Это позволит Detour A* автоматически предпочитать дороги при построении пути.

---

## Контекст

- Стандартные mmaps от TrinityCore **не различают** дороги и обычную землю
- Дороги в WoW — это **текстурные слои** в ADT файлах, не отдельная геометрия
- ADT чанк MCLY содержит до 4 текстурных слоёв, ссылающихся на MTEX (имена текстур)
- Дорожные текстуры: `*Road*`, `*Dirt*`, `*Cobble*`, `*Path*` в именах BLP файлов
- Detour `dtQueryFilter::setAreaCost()` позволяет задать стоимость по area type

---

## Подход

### 1. Модификация mmaps_generator

TrinityCore mmaps_generator — отдельный тул (~5 ключевых файлов):
- `MapBuilder.cpp` — основной пайплайн
- `TerrainBuilder.cpp` — загрузка ADT/VMap геометрии
- `IntermediateValues.cpp` — промежуточные данные

**Что изменить**:

В `TerrainBuilder::loadMap()` при обработке MCLY/MTEX:
```cpp
// Для каждого треугольника terrain mesh:
// 1. Определить доминирующий текстурный слой по позиции треугольника
// 2. Проверить имя текстуры на road-паттерны
// 3. Если дорога → пометить triangle area flag = NAV_AREA_ROAD (12)
```

В фазе `rcMarkWalkableTriangles()`:
```cpp
// Стандартно: triangleAreas[i] = RC_WALKABLE_AREA (если slope OK)
// Модификация: если texture = road → triangleAreas[i] = NAV_AREA_ROAD
```

### 2. Дорожные текстуры

Примеры текстурных имён из MPQ (WotLK):
```
Tileset\Elwynn\ElwynnDirt01.blp
Tileset\Elwynn\ElwynnDirtRoad01.blp
Tileset\Generic\GenericRoad01.blp
Tileset\Northrend\Dragonblight\DragonblightDirtRoad.blp
Tileset\Outland\Hellfire\HellfireCobblestone01.blp
Tileset\Kalimdor\Barrens\BarrensPath01.blp
```

Паттерны для матчинга (case-insensitive):
- `*road*`
- `*cobble*`
- `*path*` (осторожно — может матчить non-road текстуры)
- `*dirt*` в комбинации с `*road*`

**Важно**: нужна ручная верификация — не все `*dirt*` текстуры являются дорогами. Возможно потребуется whitelist/blacklist конкретных текстур.

### 3. Альтернативный подход: DBC-based

Другой путь определения дорог:
```
ADT MCLY → effectId → GroundEffectTexture.dbc → TerrainType.dbc
```

`TerrainType.dbc` содержит тип поверхности (для звуков шагов):
- 0 = Dirt
- 1 = Metallic
- 2 = Stone
- ...

Road/cobblestone/flagstone имеют специфические terrain types. Это более надёжно чем парсинг имён текстур.

### 4. Формат выходных файлов

Модифицированные mmtile файлы — тот же формат, но полигоны на дорогах имеют `area = NAV_AREA_ROAD` вместо `NAV_AREA_GROUND`.

В рантайме DLL:
```cpp
dtQueryFilter filter;
filter.setAreaCost(NAV_AREA_GROUND, 1.0f);
filter.setAreaCost(NAV_AREA_ROAD,   0.5f);  // дорога дешевле в 2 раза
filter.setAreaCost(NAV_AREA_WATER,  20.0f);
```

### 5. Pipeline

```
1. Скачать TrinityCore mmaps_generator source
2. Модифицировать TerrainBuilder для road detection
3. Скомпилировать тул (x64, standalone — НЕ для DLL)
4. Запустить на клиентских MPQ:
   $ mmaps_generator --offMeshInput offmesh.txt
   → /mmaps/ с модифицированными mmtile файлами
5. Положить mmtile рядом с DLL или в отдельную директорию
```

Генерация — **одноразовая**, занимает ~30-60 минут на континент.

---

## Оценка сложности

- Понимание кодовой базы mmaps_generator: **средняя**
- Модификация TerrainBuilder: **средняя** (нужно разобраться в ADT parsing)
- Тестирование/верификация: **высокая** (нужно проверить что дороги правильно размечены)
- Альтернатива (DBC): **средняя** (нужно парсить DBC файлы)

---

## Открытые вопросы

1. **Texture whitelist**: как точно определить какие текстуры являются дорогами? Нужен полный список или эвристика?
2. **DBC vs texture name**: какой подход надёжнее? DBC terrain types более структурированы
3. **Вес дороги**: 0.5 оптимально или нужна калибровка? Слишком низкий вес → бот делает огромные крюки ради дороги
4. **Тропинки**: считать тропинки (path) дорогами? Они уже, часто грунтовые
5. **Водные переправы**: некоторые дороги проходят через мелкие броды — как обработать?

---

## Критерии готовности

- [ ] Форк mmaps_generator с road detection
- [ ] Дорожные текстуры определяются автоматически (по имени или DBC)
- [ ] Модифицированные mmtile файлы генерируются
- [ ] В рантайме Detour предпочитает дороги (визуально видно на Radar)
- [ ] Калибровка весов: дорога/земля/вода
- [ ] Верификация: основные зоны (Elwynn, Barrens, Dragonblight) — пути идут по дорогам
