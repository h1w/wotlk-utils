# Task: Detour Integration + MmTile Loader

> **Status**: TODO
> **Created**: 2026-02-19
> **Phase**: 7a (Navigation)
> **Depends on**: nothing (foundational task)
> **Blocks**: nav-follow-route, nav-navigate-tool, nav-hostile-avoidance, nav-road-mmaps

---

## Цель

Интегрировать библиотеку Detour (часть Recast Navigation) в проект wotlk.dll и реализовать загрузчик TrinityCore `.mmap` / `.mmtile` файлов. Это фундамент для всей навигации — все последующие задачи зависят от этой.

---

## Контекст

- Бот использует CTM (ClickToMove) для передвижения — это уже реализовано в `game::movement`
- TrinityCore генерирует навмеш-файлы через `mmaps_generator` — мы используем готовые файлы
- Detour — runtime query библиотека (~15-20k строк C++), нужна только она (без Recast-генерации)
- Проект x86 (Win32), DLL инжектится в 32-битный WoW.exe

---

## Что нужно сделать

### 1. Подключить Detour

**Вариант A (рекомендуемый)**: vcpkg пакет `recastnavigation`
```
vcpkg install recastnavigation:x86-windows-static-md
```
Добавить .props файл аналогично `shared/minhook-static.props`.

**Вариант B**: Скопировать исходники Detour в `wotlk/src/third_party/detour/` (~10 файлов):
- `DetourAlloc.cpp/h`
- `DetourNavMesh.cpp/h`
- `DetourNavMeshQuery.cpp/h`
- `DetourNavMeshBuilder.cpp/h`
- `DetourCommon.cpp/h`
- `DetourNode.cpp/h`
- `DetourStatus.h`

### 2. Навигационный модуль

Создать `wotlk/src/navigation/` с файлами:

#### `nav_mesh.h / .cpp` — загрузка и управление навмешем

```cpp
namespace nav {

class NavMesh {
public:
    static NavMesh& Instance();

    // Инициализация: путь к директории с mmtile файлами
    bool Initialize(const char* mmapDir);
    void Shutdown();

    // Загрузка тайлов для текущего mapId
    bool LoadMap(uint32_t mapId);

    // Стриминг: загрузить/выгрузить тайлы вокруг позиции
    void UpdateLoadedTiles(float x, float y);

    // Доступ к Detour объектам
    dtNavMesh* GetNavMesh() const;
    dtNavMeshQuery* GetQuery() const;

    bool IsReady() const;

private:
    dtNavMesh*      m_navMesh = nullptr;
    dtNavMeshQuery* m_query   = nullptr;
    uint32_t        m_mapId   = 0xFFFFFFFF;
    std::string     m_mmapDir;

    // Кеш загруженных тайлов (tileX, tileY) -> dtTileRef
    std::map<std::pair<int,int>, dtTileRef> m_loadedTiles;

    // Радиус загрузки (в тайлах) — 3x3 вокруг игрока
    static constexpr int kTileLoadRadius = 1; // -1..+1 = 3x3

    bool LoadTile(int tileX, int tileY);
    void UnloadTile(int tileX, int tileY);

    // Конвертация мировых координат в тайловые
    static void WorldToTile(float x, float y, int& tileX, int& tileY);
};

} // namespace nav
```

#### `pathfinder.h / .cpp` — построение пути

```cpp
namespace nav {

struct PathResult {
    std::vector<game::Vec3> waypoints;
    bool                    success = false;
    bool                    partial = false;  // путь неполный (цель недостижима)
};

class Pathfinder {
public:
    static Pathfinder& Instance();

    // Построить путь от start до end на текущей карте
    PathResult FindPath(const game::Vec3& start, const game::Vec3& end);

    // Настройка фильтра (area costs)
    void SetAreaCost(uint8_t areaId, float cost);

private:
    dtQueryFilter m_filter;
    static constexpr int kMaxPathPolygons = 256;
    static constexpr int kMaxStraightPath = 128;
    static constexpr float kExtents[3] = { 3.0f, 5.0f, 3.0f }; // поиск ближайшего полигона
};

} // namespace nav
```

### 3. Формат файлов TrinityCore mmaps

```
Хедер карты: {mapId:04d}.mmap (например 0000.mmap для Eastern Kingdoms)
  struct MmapNavMeshHeader {  // 40 bytes
      uint32 mmapMagic;       // 0x4d4d4150 ("MMAP")
      uint32 mmapVersion;     // 16
      dtNavMeshParams params; // origin, tileWidth, tileHeight, maxTiles, maxPolys
      uint32 offmeshConnCount;
  };

Тайл: {mapId:03d}{tileY:02d}{tileX:02d}.mmtile
  struct MmapTileHeader {     // 20 bytes
      uint32 mmapMagic;       // 0x4d4d4150
      uint32 dtVersion;       // Detour version
      uint32 mmapVersion;     // 16
      uint32 size;            // Detour tile data blob size
      char   usesLiquids;
      char   padding[3];
  };
  // За хедером — raw dtNavMeshData blob (size байт)
```

### 4. Координатная система

WoW использует инвертированную систему координат:
- Мировые: X растёт на юг, Y растёт на запад
- Тайловые: `tileX = 32 - (worldX / 533.333f)`, `tileY = 32 - (worldY / 533.333f)`
- Detour findNearestPoly extents: ±3.0 XZ, ±5.0 Y
- Добавлять +0.5f к Z waypoints для предотвращения клиппинга через землю

### 5. Интеграция

- `NavMesh::Instance()` инициализируется из `overlay.cpp` или `dllmain.cpp` при входе в игру
- `UpdateLoadedTiles()` вызывается из EndScene каждые ~1 секунду (не каждый фрейм)
- Путь к mmaps: конфигурируемый (дефолт: рядом с DLL или указанная директория)
- Получение текущего mapId: `offsets::globals::MapId` (0x00AB63BC)

---

## Файловая структура

```
wotlk/src/navigation/
├── nav_mesh.h / .cpp       // dtNavMesh, загрузка mmtile, стриминг тайлов
└── pathfinder.h / .cpp     // dtNavMeshQuery, findPath, findStraightPath
```

---

## Существующие ресурсы

- **Offsets**: `offsets::globals::MapId` = 0x00AB63BC, `offsets::fn::ClickToMove` = 0x00727400
- **Game SDK**: `game::movement::ClickToMove()`, `game::Vec3`, `game::objmgr::EnumObjects()`
- **Ресерчи**: `docs/reference/researches/` — оба документа описывают mmaps пайплайн
- **TrinityCore MMapManager.cpp / PathGenerator.cpp** — референс реализации

---

## Критерии готовности

- [ ] Detour подключен и компилируется (x86 static)
- [ ] .mmap хедер читается, dtNavMesh создаётся
- [ ] .mmtile тайлы загружаются / выгружаются при движении игрока
- [ ] `Pathfinder::FindPath(A, B)` возвращает валидный массив Vec3 waypoints
- [ ] Тест: в ImGui кнопка "Find Path" → лог с количеством waypoints и общей длиной пути
