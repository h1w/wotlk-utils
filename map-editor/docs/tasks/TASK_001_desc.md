# Map Editor — Full Implementation Task

## Overview

Standalone **x64 Windows application** (NOT injection, NOT x86) — визуальный редактор навмеш-карт WoW 3.3.5a с ImGui/DX11. Позволяет просматривать навмеш, редактировать мировой граф (`world_graph.json`), создавать маршруты и тестировать Detour pathfinding.

**Финальная структура**: `map-editor/` в корне репозитория `wotlk-utils/`.

---

## Prerequisites

### vcpkg Dependencies

```bash
vcpkg install nlohmann-json:x64-windows
vcpkg install recastnavigation:x64-windows
```

Уже установлены:
- `imgui[dx11-binding,win32-binding]:x64-windows` → `C:\vcpkg\installed\x64-windows\`
- `glog:x64-windows` → `C:\vcpkg\installed\x64-windows\`

### Проверка установленных пакетов

```
C:\vcpkg\installed\x64-windows\lib\imgui.lib         (Release)
C:\vcpkg\installed\x64-windows\debug\lib\imguid.lib   (Debug)
C:\vcpkg\installed\x64-windows\lib\glog.lib
C:\vcpkg\installed\x64-windows\debug\lib\glog.lib
C:\vcpkg\installed\x64-windows\include\imgui.h
C:\vcpkg\installed\x64-windows\include\imgui_impl_dx11.h
C:\vcpkg\installed\x64-windows\include\imgui_impl_win32.h
C:\vcpkg\installed\x64-windows\include\glog\logging.h
```

После `vcpkg install` должны появиться:
```
C:\vcpkg\installed\x64-windows\include\nlohmann\json.hpp
C:\vcpkg\installed\x64-windows\include\recastnavigation\DetourNavMesh.h
C:\vcpkg\installed\x64-windows\lib\Detour.lib
C:\vcpkg\installed\x64-windows\lib\DetourTileCache.lib
```

---

## Project Structure

```
map-editor/
    map-editor.vcxproj
    src/
        main.cpp                        — WinMain, DX11 init, main loop
        app.h / app.cpp                 — App class: owns DX11, ImGui, subsystems

        canvas/
            canvas.h / canvas.cpp       — 2D pan/zoom, WoW↔screen transforms

        data/
            map_defs.h                  — Map ID constants, names
            tile_index.h / tile_index.cpp   — Scan mmaps dir → tile existence set
            world_graph_data.h / .cpp   — Load/save/edit world_graph.json
            route_data.h / route_data.cpp   — Load/save/edit custom routes JSON

        navmesh/
            tile_loader.h / tile_loader.cpp     — Load .mmtile → dtNavMesh + extract triangles
            tile_cache.h / tile_cache.cpp       — Viewport-based tile streaming + LRU cache
            pathfinder.h / pathfinder.cpp       — Detour pathfinding (FindPath between 2 points)

        editor/
            selection.h / selection.cpp         — Selection state machine
            graph_editor.h / graph_editor.cpp   — Node/edge CRUD + drag
            route_editor.h / route_editor.cpp   — Waypoint placement + drag

        render/
            grid_renderer.h / grid_renderer.cpp     — Coord grid + tile grid
            navmesh_renderer.h / navmesh_renderer.cpp — Navmesh triangle rendering
            graph_renderer.h / graph_renderer.cpp   — Graph nodes + edges
            route_renderer.h / route_renderer.cpp   — Route waypoints + lines
            path_renderer.h / path_renderer.cpp     — Test pathfinding result

        ui/
            main_menu.h / main_menu.cpp         — File/Edit/View/Map menu bar
            property_panel.h / property_panel.cpp — Inspector for selected objects
            layer_panel.h / layer_panel.cpp     — Layer visibility toggles
            status_bar.h / status_bar.cpp       — Cursor coords, zoom, map name
```

**~36 files** (18 .h + 18 .cpp включая main.cpp)

---

## Shared Props Files to Create

### `shared/imgui-dx11.props`

На основе существующего `shared/imgui-dx9.props` (x86-windows-static-md, d3d9), но для x64:

```xml
<?xml version="1.0" encoding="utf-8"?>
<Project ToolsVersion="4.0" xmlns="http://schemas.microsoft.com/developer/msbuild/2003">
  <PropertyGroup>
    <_ImguiRoot>C:\vcpkg\installed\x64-windows</_ImguiRoot>
  </PropertyGroup>
  <ItemDefinitionGroup>
    <ClCompile>
      <AdditionalIncludeDirectories>$(_ImguiRoot)\include;%(AdditionalIncludeDirectories)</AdditionalIncludeDirectories>
    </ClCompile>
    <Link>
      <AdditionalLibraryDirectories Condition="'$(Configuration)'=='Debug'">$(_ImguiRoot)\debug\lib;%(AdditionalLibraryDirectories)</AdditionalLibraryDirectories>
      <AdditionalLibraryDirectories Condition="'$(Configuration)'=='Release'">$(_ImguiRoot)\lib;%(AdditionalLibraryDirectories)</AdditionalLibraryDirectories>
      <AdditionalDependencies Condition="'$(Configuration)'=='Debug'">imguid.lib;d3d11.lib;dxgi.lib;d3dcompiler.lib;%(AdditionalDependencies)</AdditionalDependencies>
      <AdditionalDependencies Condition="'$(Configuration)'=='Release'">imgui.lib;d3d11.lib;dxgi.lib;d3dcompiler.lib;%(AdditionalDependencies)</AdditionalDependencies>
    </Link>
  </ItemDefinitionGroup>
</Project>
```

### `shared/nlohmann-json-x64.props`

Header-only, без библиотек:

```xml
<?xml version="1.0" encoding="utf-8"?>
<Project ToolsVersion="4.0" xmlns="http://schemas.microsoft.com/developer/msbuild/2003">
  <PropertyGroup>
    <_JsonRoot>C:\vcpkg\installed\x64-windows</_JsonRoot>
  </PropertyGroup>
  <ItemDefinitionGroup>
    <ClCompile>
      <AdditionalIncludeDirectories>$(_JsonRoot)\include;%(AdditionalIncludeDirectories)</AdditionalIncludeDirectories>
    </ClCompile>
  </ItemDefinitionGroup>
</Project>
```

### `shared/detour-x64.props`

На основе существующего `shared/detour-static.props`:

```xml
<?xml version="1.0" encoding="utf-8"?>
<Project ToolsVersion="4.0" xmlns="http://schemas.microsoft.com/developer/msbuild/2003">
  <PropertyGroup>
    <_DetourRoot>C:\vcpkg\installed\x64-windows</_DetourRoot>
  </PropertyGroup>
  <ItemDefinitionGroup>
    <ClCompile>
      <AdditionalIncludeDirectories>$(_DetourRoot)\include\recastnavigation;%(AdditionalIncludeDirectories)</AdditionalIncludeDirectories>
    </ClCompile>
    <Link>
      <AdditionalLibraryDirectories Condition="'$(Configuration)'=='Debug'">$(_DetourRoot)\debug\lib;%(AdditionalLibraryDirectories)</AdditionalLibraryDirectories>
      <AdditionalLibraryDirectories Condition="'$(Configuration)'=='Release'">$(_DetourRoot)\lib;%(AdditionalLibraryDirectories)</AdditionalLibraryDirectories>
      <AdditionalDependencies Condition="'$(Configuration)'=='Debug'">Detour-d.lib;DetourTileCache-d.lib;%(AdditionalDependencies)</AdditionalDependencies>
      <AdditionalDependencies Condition="'$(Configuration)'=='Release'">Detour.lib;DetourTileCache.lib;%(AdditionalDependencies)</AdditionalDependencies>
    </Link>
  </ItemDefinitionGroup>
</Project>
```

> **Примечание**: имена .lib файлов для Detour x64 могут отличаться от x86 (`Detour-d.lib` vs `Detour.lib`). Проверить после `vcpkg install recastnavigation:x64-windows` содержимое `C:\vcpkg\installed\x64-windows\lib\` и `C:\vcpkg\installed\x64-windows\debug\lib\`.

---

## vcxproj Конфигурация

### Ключевые параметры `map-editor.vcxproj`

- **Platform**: `x64` (НЕ Win32/x86)
- **ConfigurationType**: `Application`
- **SubSystem**: `Windows` (WinMain, не console)
- **LanguageStandard**: `stdcpp20`
- **RuntimeLibrary**: `MultiThreadedDebugDLL` (Debug), `MultiThreadedDLL` (Release)
- **CharacterSet**: `Unicode`
- **AdditionalOptions**: `/utf-8 /wd4996`
- **PreprocessorDefinitions**: `GLOG_USE_GLOG_EXPORT;GLOG_NO_ABBREVIATED_SEVERITIES;WIN32_LEAN_AND_MEAN;NOMINMAX`

### Подключаемые .props файлы

```xml
<Import Project="..\shared\glog-common.props" />
<Import Project="..\shared\imgui-dx11.props" />
<Import Project="..\shared\detour-x64.props" />
<Import Project="..\shared\nlohmann-json-x64.props" />
```

### glog x64 линковка

glog-common.props содержит только include/preprocessor/язык, но НЕ линковку (она per-project). Для map-editor нужно добавить:

```xml
<AdditionalLibraryDirectories Condition="'$(Configuration)'=='Debug'">C:\vcpkg\installed\x64-windows\debug\lib;%(AdditionalLibraryDirectories)</AdditionalLibraryDirectories>
<AdditionalLibraryDirectories Condition="'$(Configuration)'=='Release'">C:\vcpkg\installed\x64-windows\lib;%(AdditionalLibraryDirectories)</AdditionalLibraryDirectories>
<AdditionalDependencies>glog.lib;%(AdditionalDependencies)</AdditionalDependencies>
```

Также include:
```xml
<AdditionalIncludeDirectories>C:\vcpkg\installed\x64-windows\include;%(AdditionalIncludeDirectories)</AdditionalIncludeDirectories>
```

### Post-Build: копирование DLL

imgui и glog — dynamic linking для x64-windows. Нужно копировать DLL рядом с exe:

```xml
<PostBuildEvent>
  <Command>
    xcopy /Y /D "C:\vcpkg\installed\x64-windows\bin\imgui.dll" "$(OutDir)"
    xcopy /Y /D "C:\vcpkg\installed\x64-windows\bin\glog.dll" "$(OutDir)"
    xcopy /Y /D "C:\vcpkg\installed\x64-windows\bin\gflags.dll" "$(OutDir)"
  </Command>
  <!-- Debug -->
  <Command Condition="'$(Configuration)'=='Debug'">
    xcopy /Y /D "C:\vcpkg\installed\x64-windows\debug\bin\imguid.dll" "$(OutDir)"
    xcopy /Y /D "C:\vcpkg\installed\x64-windows\debug\bin\glog.dll" "$(OutDir)"
    xcopy /Y /D "C:\vcpkg\installed\x64-windows\debug\bin\gflags_debug.dll" "$(OutDir)"
  </Command>
</PostBuildEvent>
```

### Добавление в Solution

В `wotlk-utils.sln` добавить новый проект с новым GUID (сгенерировать). Solution configurations: `Debug|x64` и `Release|x64` — проект map-editor собирается ТОЛЬКО для x64. Для x86 конфигураций — `ActiveCfg` без `Build.0` (не собирается).

---

## Phase 1: Skeleton App + Canvas + Tile Grid

### Цель
Окно DX11/ImGui, панорамируемый/зумируемый холст, сетка тайлов, статус-бар.

### Файлы
`main.cpp`, `app.h/.cpp`, `canvas/canvas.h/.cpp`, `data/map_defs.h`, `data/tile_index.h/.cpp`, `render/grid_renderer.h/.cpp`, `ui/status_bar.h/.cpp`

### Детали реализации

#### `main.cpp` — WinMain + DX11 init + main loop

```cpp
#include "app.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int nCmdShow) {
    App app;
    if (!app.Initialize(hInstance, nCmdShow))
        return 1;
    app.Run();  // Main loop — blocks until window closed
    app.Shutdown();
    return 0;
}
```

#### `app.h / app.cpp` — DX11 + ImGui lifecycle

Управляет:
- HWND creation (WS_OVERLAPPEDWINDOW, 1600x900)
- ID3D11Device + ID3D11DeviceContext + IDXGISwapChain
- ImGui context init (ImGui_ImplWin32_Init + ImGui_ImplDX11_Init)
- Render target view (recreate on resize)
- Main loop: PeekMessage → ImGui NewFrame → Update → Render → Present

**DX11 init pattern** (стандартный ImGui example):
```cpp
DXGI_SWAP_CHAIN_DESC sd = {};
sd.BufferCount = 2;
sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
sd.OutputWindow = hWnd;
sd.SampleDesc.Count = 1;
sd.Windowed = TRUE;
sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
    nullptr, 0, D3D11_SDK_VERSION, &sd, &swapChain, &device, nullptr, &context);
```

**WndProc** должен:
1. Forward WM messages в `ImGui_ImplWin32_WndProcHandler`
2. Handle WM_SIZE — вызвать resize callback (пересоздать render target)
3. Handle WM_DESTROY → PostQuitMessage(0)

```cpp
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);
```

**ImGui стиль**: тёмная тема (`ImGui::StyleColorsDark()`)

**Main loop**:
```
while (running) {
    MSG msg;
    while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg); DispatchMessage(&msg);
        if (msg.message == WM_QUIT) running = false;
    }
    // NewFrame
    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();

    // App logic: render canvas, UI panels, etc.
    canvas.Update();
    gridRenderer.Render(canvas);
    statusBar.Render(canvas);

    // DX11 render
    context->OMSetRenderTargets(1, &rtv, nullptr);
    float clear[4] = {0.1f, 0.1f, 0.12f, 1.0f};
    context->ClearRenderTargetView(rtv, clear);

    ImGui::Render();
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    swapChain->Present(1, 0);  // vsync
}
```

#### `canvas/canvas.h/.cpp` — Pan/Zoom 2D Canvas

**Ключевые поля**:
```cpp
struct Canvas {
    // Camera center in WoW world coordinates
    float centerX = -8800.0f;  // Default: near Stormwind
    float centerY = 500.0f;

    // Zoom: pixels per WoW yard
    float zoom = 0.5f;
    static constexpr float kMinZoom = 0.001f;  // continent view
    static constexpr float kMaxZoom = 10.0f;    // close-up

    // Screen-space canvas bounds (set each frame from ImGui)
    ImVec2 canvasPos;   // top-left screen pos
    ImVec2 canvasSize;  // width, height in pixels

    // Pan state
    bool isPanning = false;
    ImVec2 panStartMouse;
    float panStartCenterX, panStartCenterY;
};
```

**Coordinate Transform: WoW → Screen**

WoW: X+=south, Y+=west. Для north-up карты:
- Screen right = East = WoW **-Y**
- Screen down = South = WoW **+X**

```cpp
ImVec2 WorldToScreen(float wowX, float wowY) const {
    float sx = canvasPos.x + canvasSize.x * 0.5f + (centerY - wowY) * zoom;
    float sy = canvasPos.y + canvasSize.y * 0.5f + (wowX - centerX) * zoom;
    return {sx, sy};
}

void ScreenToWorld(float sx, float sy, float& wowX, float& wowY) const {
    wowX = centerX + (sy - canvasPos.y - canvasSize.y * 0.5f) / zoom;
    wowY = centerY - (sx - canvasPos.x - canvasSize.x * 0.5f) / zoom;
}
```

**Input handling** (каждый кадр):
```
Update():
    // Canvas occupies main area (dock remaining after panels)
    canvasPos = ImGui::GetCursorScreenPos()
    canvasSize = ImGui::GetContentRegionAvail()
    ImGui::InvisibleButton("canvas", canvasSize)
    hovered = ImGui::IsItemHovered()

    if (hovered) {
        // Middle-click pan
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Middle)) {
            isPanning = true; panStartMouse = mousePos; panStartCenter = center;
        }
        // Zoom with scroll wheel (centered on cursor)
        float wheel = ImGui::GetIO().MouseWheel;
        if (wheel != 0) {
            // Before zoom: cursor world pos
            ScreenToWorld(mousePos, cursorWX, cursorWY);
            zoom *= (wheel > 0) ? 1.15f : (1.0f / 1.15f);
            zoom = clamp(zoom, kMinZoom, kMaxZoom);
            // After zoom: adjust center so cursor stays at same world pos
            // (centerY - wowY) * newZoom = screenOffsetX => centerY = wowY + screenOffsetX / newZoom
            // Реализация: пересчитать centerX/centerY чтобы cursorWX/WY осталось на месте
        }
    }
    if (isPanning) {
        if (ImGui::IsMouseDown(ImGuiMouseButton_Middle)) {
            ImVec2 delta = mousePos - panStartMouse;
            // delta.x = screen right = WoW -Y direction
            // delta.y = screen down = WoW +X direction
            centerX = panStartCenterX - delta.y / zoom;
            centerY = panStartCenterY + delta.x / zoom;
        } else {
            isPanning = false;
        }
    }
```

**Viewport bounds** (для tile streaming/culling):
```cpp
void GetViewportWorldBounds(float& minX, float& maxX, float& minY, float& maxY) const {
    ScreenToWorld(canvasPos.x, canvasPos.y, ???, ???);  // top-left corner
    ScreenToWorld(canvasPos.x + canvasSize.x, canvasPos.y + canvasSize.y, ???, ???); // bottom-right
    // min/max across all 4 corners
}
```

#### `data/map_defs.h` — Map ID Constants

```cpp
#pragma once
#include <cstdint>

namespace mapdef {

struct MapInfo {
    uint32_t id;
    const char* name;
    const char* shortName;
};

inline constexpr MapInfo kMaps[] = {
    {  0,   "Eastern Kingdoms",  "EK"        },
    {  1,   "Kalimdor",          "Kalimdor"  },
    { 530,  "Outland",           "Outland"   },
    { 571,  "Northrend",         "Northrend" },
    {  33,  "Shadowfang Keep",   "SFK"       },
    {  34,  "The Stockade",      "Stockade"  },
    {  36,  "Deadmines",         "DM"        },
    {  43,  "Wailing Caverns",   "WC"        },
    {  47,  "Razorfen Kraul",    "RFK"       },
    {  48,  "Blackfathom Deeps", "BFD"       },
    {  70,  "Uldaman",           "Ulda"      },
    {  90,  "Gnomeregan",        "Gnomer"    },
    { 109,  "Sunken Temple",     "ST"        },
    { 129,  "Razorfen Downs",    "RFD"       },
    { 189,  "Scarlet Monastery", "SM"        },
    { 209,  "Zul'Farrak",        "ZF"        },
    { 229,  "Blackrock Spire",   "BRS"       },
    { 230,  "Blackrock Depths",  "BRD"       },
    { 249,  "Onyxia's Lair",     "Onyxia"   },
    { 269,  "Black Morass",      "BM"        },
    { 289,  "Scholomance",       "Scholo"    },
    { 309,  "Zul'Gurub",         "ZG"        },
    { 329,  "Stratholme",        "Strat"     },
    { 349,  "Maraudon",          "Mara"      },
    { 389,  "Ragefire Chasm",    "RFC"       },
    { 409,  "Molten Core",       "MC"        },
    { 429,  "Dire Maul",         "DireMaul"  },
    { 469,  "Blackwing Lair",    "BWL"       },
    { 509,  "Ruins of Ahn'Qiraj","AQ20"     },
    { 531,  "Temple of Ahn'Qiraj","AQ40"    },
    { 532,  "Karazhan",          "Kara"      },
    { 540,  "Hellfire Ramparts", "Ramps"     },
    { 542,  "Blood Furnace",     "BF"        },
    { 543,  "Hellfire Citadel",  "HFC"       },
    { 544,  "Magtheridon's Lair","Magth"     },
    { 545,  "Steamvault",        "SV"        },
    { 546,  "Underbog",          "UB"        },
    { 547,  "Slave Pens",        "SP"        },
    { 548,  "Serpentshrine Cavern","SSC"     },
    { 550,  "Tempest Keep",      "TK"        },
    { 552,  "The Arcatraz",      "Arca"      },
    { 553,  "The Botanica",      "Bot"       },
    { 554,  "The Mechanar",      "Mech"      },
    { 555,  "Shadow Labyrinth",  "SLab"      },
    { 556,  "Sethekk Halls",     "SH"        },
    { 557,  "Mana-Tombs",        "MT"        },
    { 558,  "Auchenai Crypts",   "AC"        },
    { 560,  "Old Hillsbrad",     "OHB"       },
    { 564,  "Black Temple",      "BT"        },
    { 565,  "Gruul's Lair",      "Gruul"     },
    { 568,  "Zul'Aman",          "ZA"        },
    { 574,  "Utgarde Keep",      "UK"        },
    { 575,  "Utgarde Pinnacle",  "UP"        },
    { 576,  "The Nexus",         "Nexus"     },
    { 578,  "The Oculus",        "Occ"       },
    { 595,  "Culling of Stratholme","CoS"   },
    { 599,  "Halls of Stone",    "HoS"       },
    { 600,  "Drak'Tharon Keep",  "DTK"       },
    { 601,  "Azjol-Nerub",       "AN"        },
    { 602,  "Halls of Lightning","HoL"       },
    { 603,  "Ulduar",            "Ulduar"    },
    { 604,  "Gundrak",           "Gun"       },
    { 608,  "Violet Hold",       "VH"        },
    { 615,  "Obsidian Sanctum",  "OS"        },
    { 616,  "Eye of Eternity",   "EoE"       },
    { 619,  "Ahn'kahet",         "AK"        },
    { 624,  "Vault of Archavon", "VoA"       },
    { 631,  "Icecrown Citadel",  "ICC"       },
    { 632,  "Forge of Souls",    "FoS"       },
    { 649,  "Trial of the Crusader","ToC"   },
    { 650,  "Trial of the Champion","ToChamp"},
    { 658,  "Pit of Saron",      "PoS"       },
    { 668,  "Halls of Reflection","HoR"     },
    { 724,  "Ruby Sanctum",      "RS"        },
};

inline constexpr int kMapCount = sizeof(kMaps) / sizeof(kMaps[0]);

// Find map info by ID. Returns nullptr if not found.
inline const MapInfo* FindMap(uint32_t id) {
    for (int i = 0; i < kMapCount; ++i)
        if (kMaps[i].id == id) return &kMaps[i];
    return nullptr;
}

} // namespace mapdef
```

#### `data/tile_index.h/.cpp` — Tile Existence Scanner

Сканирует директорию mmaps и строит `set<pair<int,int>>` для каждого mapId.

Формат имён файлов TC mmtile: `{mapId:03d}{tileX:02d}{tileY:02d}.mmtile`

```cpp
// tile_index.h
#pragma once
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <utility>

class TileIndex {
public:
    // Scan directory for .mmtile files and build tile index
    bool ScanDirectory(const std::string& mmapDir);

    // Check if a tile exists for given map
    bool HasTile(uint32_t mapId, int tileX, int tileY) const;

    // Get all tiles for a map
    const std::set<std::pair<int,int>>* GetTilesForMap(uint32_t mapId) const;

    // Get all map IDs that have at least one tile
    std::vector<uint32_t> GetAvailableMaps() const;

    // Get tile count for a map
    int GetTileCount(uint32_t mapId) const;

    const std::string& GetMmapDir() const { return m_mmapDir; }

private:
    std::string m_mmapDir;
    std::map<uint32_t, std::set<std::pair<int,int>>> m_tiles;
};
```

**Реализация** `ScanDirectory`: итерировать `std::filesystem::directory_iterator(mmapDir)`, для каждого файла `*.mmtile` → parse mapId (3 цифры), tileX (2 цифры), tileY (2 цифры) из имени.

#### `render/grid_renderer.h/.cpp` — Coordinate + Tile Grid

Рисует на ImDrawList:
1. **Координатную сетку**: адаптивный шаг (при zoom < 0.01 → 1000 yd, при zoom < 0.1 → 100 yd, иначе 10 yd). Тонкие серые линии.
2. **Сетку тайлов**: каждый тайл — прямоугольник 533.33 × 533.33 yd. Существующие тайлы (из TileIndex) — зелёные контуры. Отсутствующие — ничего.

**Tile coordinate ↔ World coordinate**:

Формула из `nav_mesh.cpp:WorldToTile`:
```cpp
tileX = 32 - floor(wowX / 533.33333f)
tileY = 32 - floor(wowY / 533.33333f)
```

Обратная:
```cpp
wowX_min = (32 - tileX) * 533.33333f         // south edge
wowX_max = (32 - tileX + 1) * 533.33333f     // north edge (lower wowX)
// Wait — tileX increases as wowX decreases. So:
// tileX corresponds to wowX range: [(32-tileX)*TILE_SIZE, (33-tileX)*TILE_SIZE]
// Actually: floor(wowX/TILE) = 32 - tileX → wowX in [TILE*(32-tileX), TILE*(33-tileX)]
wowX_min = (32 - tileX) * TILE_SIZE
wowX_max = (33 - tileX) * TILE_SIZE
wowY_min = (32 - tileY) * TILE_SIZE
wowY_max = (33 - tileY) * TILE_SIZE
```

> **ВАЖНО**: `TILE_SIZE = 533.33333f` (одно из ключевых значений в проекте, из `nav_mesh.cpp`)

**Рисование**:
```cpp
void GridRenderer::Render(const Canvas& canvas, const TileIndex& tileIndex, uint32_t mapId) {
    ImDrawList* dl = ImGui::GetWindowDrawList();

    // Determine visible world bounds
    float wMinX, wMaxX, wMinY, wMaxY;
    canvas.GetViewportWorldBounds(wMinX, wMaxX, wMinY, wMaxY);

    // 1. Coord grid
    float gridStep = (canvas.zoom < 0.01f) ? 1000.0f : (canvas.zoom < 0.1f) ? 100.0f : 10.0f;
    // ... draw vertical and horizontal lines

    // 2. Tile grid
    auto* tiles = tileIndex.GetTilesForMap(mapId);
    if (!tiles) return;
    for (auto& [tx, ty] : *tiles) {
        float x0 = (32 - tx) * TILE_SIZE;
        float x1 = (33 - tx) * TILE_SIZE;
        float y0 = (32 - ty) * TILE_SIZE;
        float y1 = (33 - ty) * TILE_SIZE;
        ImVec2 p0 = canvas.WorldToScreen(x0, y0);
        ImVec2 p1 = canvas.WorldToScreen(x1, y1);
        dl->AddRectFilled(p0, p1, IM_COL32(30, 80, 30, 60));
        dl->AddRect(p0, p1, IM_COL32(40, 120, 40, 100));
    }
}
```

#### `ui/status_bar.h/.cpp`

Горизонтальная полоса внизу окна:
- Координаты курсора в WoW: `X: -8840.6  Y: 489.7`
- Текущий zoom: `Zoom: 0.50 px/yd`
- Текущая карта: `Map: Eastern Kingdoms (0)`
- Tile под курсором: `Tile: (32, 31)`

### Проверка Phase 1
Запустить `map-editor.exe` → чёрное окно с ImGui → виден тайл-грид Eastern Kingdoms (зелёные квадраты) → средней кнопкой мыши панорамирование → колесом зум → в статус-баре координаты обновляются → dropdown для переключения карт работает.

---

## Phase 2: Navmesh Polygon Rendering

### Цель
Отображение walkable-зон из .mmtile файлов — полноценная навмеш-карта.

### Файлы
`navmesh/tile_loader.h/.cpp`, `navmesh/tile_cache.h/.cpp`, `render/navmesh_renderer.h/.cpp`

### Детали реализации

#### `navmesh/tile_loader.h/.cpp` — Load mmtile + Extract Triangles

**Задача**: загрузить один .mmtile файл → dtNavMesh::addTile → извлечь detail mesh треугольники → вернуть vertex data для рендеринга.

**Структуры** (копируются из `nav_mesh.cpp`):
```cpp
#pragma pack(push, 1)
struct MmapTileHeader {
    uint32_t mmapMagic;    // 0x4D4D4150
    uint32_t dtVersion;
    uint32_t mmapVersion;
    uint32_t size;
    char     usesLiquids;
    char     padding[3];
};
#pragma pack(pop)

static constexpr uint32_t MMAP_MAGIC = 0x4D4D4150;
static constexpr float TILE_SIZE = 533.33333f;
```

**Repack logic** — точная копия из `nav_mesh.cpp:LoadTile()` строки 298-399:
- Вычислить `preLinksSz`, `tcLinksSz` (16-byte dtLink), `stockLinksSz` (12-byte dtLink), `postLinksSz`
- Если `tcLinksSz != stockLinksSz` → allocate repacked buffer → copy header+verts+polys → zero links → copy post-links → dtFree old data
- Константа: `kTcDtLinkSize = 16`

**Triangle extraction** из загруженного dtMeshTile:
```cpp
struct TileTriangles {
    // Vertices as 2D WoW coords for rendering (Z stored for height coloring)
    struct Vertex { float wowX, wowY, wowZ; };
    std::vector<Vertex> vertices;    // 3 vertices per triangle (sequential)
    // Bounding box in WoW coords
    float minX, maxX, minY, maxY;
};

TileTriangles ExtractTriangles(const dtNavMesh* mesh, int tileX, int tileY) {
    TileTriangles result;
    const dtMeshTile* tile = mesh->getTileAt(tileX, tileY, 0);
    if (!tile || !tile->header) return result;

    const dtMeshHeader* hdr = tile->header;

    for (int i = 0; i < hdr->polyCount; ++i) {
        const dtPoly& poly = tile->polys[i];
        if (poly.getType() == DT_POLYTYPE_OFFMESH_CONNECTION) continue;

        const dtPolyDetail& detail = tile->detailMeshes[i];

        for (unsigned int j = 0; j < detail.triCount; ++j) {
            const unsigned char* t = &tile->detailTris[(detail.triBase + j) * 4];

            for (int k = 0; k < 3; ++k) {
                float detourPos[3];
                if (t[k] < poly.vertCount) {
                    // Poly vertex
                    dtVcopy(detourPos, &tile->verts[poly.verts[t[k]] * 3]);
                } else {
                    // Detail vertex
                    dtVcopy(detourPos, &tile->detailVerts[(detail.vertBase + t[k] - poly.vertCount) * 3]);
                }

                // Detour → WoW coords:
                // Detour[0]=WowY, Detour[1]=WowZ, Detour[2]=WowX
                TileTriangles::Vertex v;
                v.wowX = detourPos[2];  // WoW X = Detour[2]
                v.wowY = detourPos[0];  // WoW Y = Detour[0]
                v.wowZ = detourPos[1];  // WoW Z = Detour[1]
                result.vertices.push_back(v);
            }
        }
    }

    // Compute bounding box
    // ...

    return result;
}
```

**dtNavMesh init** — тот же фикс для 32-bit dtPolyRef что и в `nav_mesh.cpp:LoadMap()`:
```cpp
// Для map editor нужен больший maxTiles (может показывать всю карту)
params.maxTiles = 4096;  // enough for full continent
// polyBits: 32 - tileBits(12) - saltBits(10) = 10 → maxPolys = 1024
// Или: maxTiles = 1024, tileBits = 10, polyBits = 12, maxPolys = 4096
// Подобрать под нужды: 200 cached tiles → maxTiles = 256
```

> **ВАЖНО**: maxTiles для map editor должен быть больше чем для DLL (64), потому что кэш может содержать 200+ тайлов. Установить `maxTiles = 512`, тогда tileBits=9, polyBits=13, maxPolys=8192.

#### `navmesh/tile_cache.h/.cpp` — Viewport Streaming + LRU Cache

```cpp
class TileCache {
public:
    // Set mmaps directory and create dtNavMesh for a map
    bool Initialize(const std::string& mmapDir, uint32_t mapId);
    void Shutdown();

    // Update visible tiles based on viewport bounds
    // Loads new tiles, evicts old ones via LRU
    void UpdateViewport(float minWowX, float maxWowX, float minWowY, float maxWowY, float zoom);

    // Get triangles for rendering
    const std::map<std::pair<int,int>, TileTriangles>& GetLoadedTriangles() const;

    // Access dtNavMesh and dtNavMeshQuery (for pathfinding)
    dtNavMesh* GetNavMesh() { return m_navMesh; }
    dtNavMeshQuery* GetQuery() { return m_query; }

    int GetLoadedTileCount() const;

private:
    static constexpr int kMaxCachedTiles = 200;

    dtNavMesh* m_navMesh = nullptr;
    dtNavMeshQuery* m_query = nullptr;
    std::string m_mmapDir;
    uint32_t m_mapId = 0xFFFFFFFF;

    struct CachedTile {
        TileTriangles triangles;
        dtTileRef tileRef = 0;
        uint64_t lastUsedFrame = 0;
    };

    std::map<std::pair<int,int>, CachedTile> m_cache;
    uint64_t m_frameCounter = 0;

    bool LoadTile(int tileX, int tileY);
    void EvictLRU();
};
```

**Алгоритм UpdateViewport**:
1. Вычислить видимые тайлы из viewport bounds → `WorldToTile` для 4 углов viewport
2. Для каждого видимого тайла: если не в кэше → LoadTile
3. Обновить `lastUsedFrame` для всех видимых тайлов
4. Если `m_cache.size() > kMaxCachedTiles` → EvictLRU (удалить тайлы с наименьшим lastUsedFrame)
5. Инкремент `m_frameCounter`

**WorldToTile** (та же формула что в DLL):
```cpp
static void WorldToTile(float x, float y, int& tileX, int& tileY) {
    tileX = 32 - static_cast<int>(std::floor(x / 533.33333f));
    tileY = 32 - static_cast<int>(std::floor(y / 533.33333f));
}
```

#### `render/navmesh_renderer.h/.cpp`

Рисует треугольники из TileCache через ImDrawList.

```cpp
void NavmeshRenderer::Render(const Canvas& canvas, const TileCache& cache) {
    ImDrawList* dl = ImGui::GetWindowDrawList();

    // LOD: при zoom < 0.05 — не рисуем полигоны (tile grid достаточно)
    if (canvas.zoom < 0.05f) return;

    float minX, maxX, minY, maxY;
    canvas.GetViewportWorldBounds(minX, maxX, minY, maxY);

    for (auto& [key, triData] : cache.GetLoadedTriangles()) {
        // Frustum cull: skip tiles completely outside viewport
        if (triData.maxX < minX || triData.minX > maxX ||
            triData.maxY < minY || triData.minY > maxY)
            continue;

        // Draw filled triangles
        for (size_t i = 0; i + 2 < triData.vertices.size(); i += 3) {
            ImVec2 p0 = canvas.WorldToScreen(triData.vertices[i].wowX, triData.vertices[i].wowY);
            ImVec2 p1 = canvas.WorldToScreen(triData.vertices[i+1].wowX, triData.vertices[i+1].wowY);
            ImVec2 p2 = canvas.WorldToScreen(triData.vertices[i+2].wowX, triData.vertices[i+2].wowY);

            dl->AddTriangleFilled(p0, p1, p2, IM_COL32(30, 100, 40, 80));
            dl->AddTriangle(p0, p1, p2, IM_COL32(40, 130, 50, 120));
        }
    }
}
```

> **Оптимизация**: при большом количестве треугольников (>100k) ImDrawList может тормозить. Если будет проблема — можно:
> 1. Per-tile culling (уже есть)
> 2. Per-triangle screen-space culling (skip if all 3 points outside viewport)
> 3. Batch vertices в один AddConvexPolyFilled call (less draw commands)
> 4. Рендерить в текстуру (D3D11) и показывать как quad (самый быстрый, но сложнее)

### Проверка Phase 2
Зумировать в Elwynn Forest (WoW X ~ -9000...-8500, Y ~ 300...1000) → увидеть зелёные навмеш-полигоны → при отдалении (zoom < 0.05) полигоны исчезают, видны только tile-прямоугольники.

---

## Phase 3: World Graph Display + Editing

### Цель
Загрузка, отображение и полное редактирование world_graph.json.

### Файлы
`data/world_graph_data.h/.cpp`, `render/graph_renderer.h/.cpp`, `editor/graph_editor.h/.cpp`, `editor/selection.h/.cpp`, `ui/property_panel.h/.cpp`, `ui/layer_panel.h/.cpp`, `ui/main_menu.h/.cpp`

### Детали реализации

#### `data/world_graph_data.h/.cpp`

Аналог `world_graph.h/cpp` из DLL, но с полной CRUD-поддержкой и сохранением.

**JSON формат** (из `wotlk/docs/game_data/world_graph.json`):
```json
{
  "nodes": [
    {"id": 2, "name": "Stormwind, Elwynn", "map": 0, "x": -8840.6, "y": 489.7, "z": 109.6,
     "type": "flight_master", "faction": "alliance"},
    ...
  ],
  "edges": [
    {"from": 2, "to": 4, "type": "flight", "cost": 72.5, "bidir": false},
    ...
  ]
}
```

**Типы нод** (JSON string → enum):
- `"waypoint"` → Waypoint
- `"flight_master"` → FlightMaster
- `"portal"` → Portal
- `"boat_zeppelin"` → BoatZeppelin
- `"innkeeper"` → InnKeeper
- `"zone_boundary"` → ZoneBoundary
- `"dungeon"` → DungeonEntrance

**Типы рёбер**:
- `"walk"` → Walk
- `"flight"` → Flight
- `"teleport"` → Teleport
- `"boat"` → Boat

**Фракции** (поле `"faction"` у нод):
- `"alliance"`, `"horde"`, `"neutral"`, `""` (пусто = не задана)

**CRUD операции**:
```cpp
class WorldGraphData {
public:
    bool LoadFromFile(const std::string& path);
    bool SaveToFile(const std::string& path) const;

    // Node CRUD
    uint32_t AddNode(const WorldNode& node);  // returns assigned ID
    void RemoveNode(uint32_t id);             // also removes connected edges
    void UpdateNode(uint32_t id, const WorldNode& node);
    WorldNode* GetNode(uint32_t id);

    // Edge CRUD
    void AddEdge(const WorldEdge& edge);
    void RemoveEdge(size_t index);
    void UpdateEdge(size_t index, const WorldEdge& edge);

    // Access
    const std::vector<WorldNode>& GetNodes() const;
    const std::vector<WorldEdge>& GetEdges() const;

    // For editing
    std::vector<WorldNode>& GetNodesMut();
    std::vector<WorldEdge>& GetEdgesMut();

    // Dirty tracking
    bool IsDirty() const { return m_dirty; }
    void ClearDirty() { m_dirty = false; }

private:
    std::vector<WorldNode> m_nodes;
    std::vector<WorldEdge> m_edges;
    uint32_t m_nextId = 1;
    bool m_dirty = false;
    std::string m_filePath;
};
```

**SaveToFile** — записать тот же JSON формат. Сортировать ноды по ID, рёбра по (from, to). Использовать `json.dump(2)` для красивого форматирования.

#### `editor/selection.h/.cpp` — Selection State Machine

```cpp
enum class SelectionType { None, Node, Edge, Waypoint };

struct Selection {
    SelectionType type = SelectionType::None;
    uint32_t nodeId = 0;        // for Node selection
    size_t edgeIndex = 0;       // for Edge selection
    int routeIndex = -1;        // for Waypoint selection (which route)
    int waypointIndex = -1;     // for Waypoint selection (which waypoint)

    void Clear() { type = SelectionType::None; }
    bool IsNode() const { return type == SelectionType::Node; }
    bool IsEdge() const { return type == SelectionType::Edge; }
    bool IsWaypoint() const { return type == SelectionType::Waypoint; }
};
```

#### `editor/graph_editor.h/.cpp` — Node/Edge Editing

**Режимы**:
- **Normal**: click=select node/edge, drag=move selected node, Delete=remove selected
- **Add Node** (double-click): добавить ноду в точку клика
- **Edge Creation** (нажать E): click first node → click second node → create edge

**Input handling**:
```
if (hovered canvas) {
    // Hit test: find closest node within 10px
    WorldNode* hitNode = FindNodeAtScreen(mousePos, 10.0f);
    WorldEdge* hitEdge = FindEdgeAtScreen(mousePos, 5.0f);

    if (left click) {
        if (edgeCreationMode) {
            if (hitNode) {
                if (!firstNode) firstNode = hitNode;
                else { CreateEdge(firstNode, hitNode); edgeCreationMode = false; }
            }
        } else {
            if (hitNode) selection.SelectNode(hitNode->id);
            else if (hitEdge) selection.SelectEdge(edgeIndex);
            else selection.Clear();
        }
    }

    if (double click && !hitNode) {
        // Add new node at click position
        WorldNode node;
        node.mapId = currentMapId;
        canvas.ScreenToWorld(mousePos, node.pos.x, node.pos.y);
        node.pos.z = 0;  // Z unknown without navmesh raycast
        graph.AddNode(node);
    }

    if (dragging selected node) {
        canvas.ScreenToWorld(mousePos, node->pos.x, node->pos.y);
        graph.MarkDirty();
    }

    if (Delete pressed && selection.IsNode()) {
        graph.RemoveNode(selection.nodeId);
        selection.Clear();
    }
}
```

#### `render/graph_renderer.h/.cpp`

**Цвета нод по типу**:
```cpp
ImU32 GetNodeColor(NodeType type) {
    switch (type) {
        case NodeType::FlightMaster:    return IM_COL32(50, 200, 50, 255);   // green
        case NodeType::Waypoint:        return IM_COL32(200, 200, 200, 255); // white/gray
        case NodeType::Portal:          return IM_COL32(180, 50, 220, 255);  // purple
        case NodeType::BoatZeppelin:    return IM_COL32(80, 180, 255, 255);  // light blue
        case NodeType::InnKeeper:       return IM_COL32(255, 200, 0, 255);   // yellow
        case NodeType::DungeonEntrance: return IM_COL32(180, 100, 40, 255);  // brown
        case NodeType::ZoneBoundary:    return IM_COL32(150, 150, 150, 255); // gray
        default:                        return IM_COL32(255, 255, 255, 255);
    }
}
```

**Цвета рёбер по типу**:
```cpp
ImU32 GetEdgeColor(EdgeType type) {
    switch (type) {
        case EdgeType::Walk:     return IM_COL32(150, 150, 150, 150);  // gray
        case EdgeType::Flight:   return IM_COL32(50, 200, 50, 200);    // green (dashed)
        case EdgeType::Teleport: return IM_COL32(180, 50, 220, 200);   // purple
        case EdgeType::Boat:     return IM_COL32(80, 180, 255, 200);   // light blue
        default:                 return IM_COL32(255, 255, 255, 150);
    }
}
```

**Рисование нод**: кружок радиусом 5-8px (зависит от zoom), с подписью имени при zoom > 0.3.
**Рисование рёбер**: линия между нодами. Стрелка на конце если `bidir == false`. Flight рёбра — пунктиром (чередуя отрезки). Толщина 1-2px.
**Выделение**: выбранная нода/ребро — яркий жёлтый контур, увеличенный радиус.

**Рисование подписей нод** при достаточном zoom:
```cpp
if (canvas.zoom > 0.3f) {
    ImVec2 textPos(screenPos.x + 10, screenPos.y - 5);
    dl->AddText(textPos, IM_COL32(255, 255, 255, 200), node.name.c_str());
}
```

#### `ui/property_panel.h/.cpp` — Inspector

Правая панель ~250px. Показывает и позволяет редактировать свойства выбранного объекта.

**Для ноды**:
- ID (read-only)
- Name (InputText)
- Map (dropdown)
- X, Y, Z (DragFloat)
- Type (dropdown: Waypoint, FlightMaster, Portal, etc.)
- Faction (dropdown: Alliance, Horde, Neutral)

**Для ребра**:
- From → To (node IDs, read-only, кликабельные для перехода к ноде)
- Type (dropdown: Walk, Flight, Teleport, Boat)
- Cost (DragFloat)
- Bidirectional (Checkbox)

#### `ui/layer_panel.h/.cpp` — Layer Visibility

Левая панель или часть правой. Чекбоксы:
- [ ] Navmesh polygons
- [ ] Tile grid
- [ ] Coord grid
- [ ] Graph: FlightMasters
- [ ] Graph: Waypoints
- [ ] Graph: Portals
- [ ] Graph: Boats
- [ ] Graph: Walk edges
- [ ] Graph: Flight edges
- [ ] Graph: Teleport edges
- [ ] Routes
- [ ] Test Path

#### `ui/main_menu.h/.cpp` — Menu Bar

```
File | Edit | View | Map
```

**File**: Open Graph (Ctrl+O), Save Graph (Ctrl+S), Save As... (Ctrl+Shift+S), Open Routes, Save Routes, separator, Exit
**Edit**: Undo (Ctrl+Z) [optional], Redo (Ctrl+Y) [optional], separator, Delete Selected (Del)
**View**: Toggle Layer Panel, Toggle Property Panel, Reset Zoom, Zoom to Fit
**Map**: Radio buttons для каждой карты (Eastern Kingdoms, Kalimdor, Outland, Northrend, + dungeons что есть в TileIndex)

**Файловые диалоги**: использовать Win32 `GetOpenFileNameA` / `GetSaveFileNameA` для выбора файлов.

**Dirty flag**: если есть несохранённые изменения и пользователь закрывает окно или открывает другой файл → показать MessageBox "Unsaved changes. Save?"

### Проверка Phase 3
File → Open → `wotlk/docs/game_data/world_graph.json` → видны ноды на карте с подписями → зелёные кружки flight masters, белые waypoints → рёбра между ними → кликнуть ноду → в property panel появились свойства → перетащить ноду → Save → проверить что JSON обновился.

---

## Phase 4: Custom Route Editor

### Цель
Создание и редактирование пользовательских маршрутов (farming routes, patrol routes).

### Файлы
`data/route_data.h/.cpp`, `editor/route_editor.h/.cpp`, `render/route_renderer.h/.cpp`

### JSON формат `routes.json`

```json
{
  "routes": [
    {
      "name": "Westfall Farm",
      "map": 0,
      "loop": true,
      "color": "#FFC800",
      "waypoints": [
        {"x": -10629.3, "y": 1036.9, "z": 34.0},
        {"x": -10580.0, "y": 1120.5, "z": 37.2, "action": "wait", "delay": 2.0},
        {"x": -10500.0, "y": 1050.0, "z": 35.5}
      ]
    },
    {
      "name": "Elwynn Mining",
      "map": 0,
      "loop": true,
      "color": "#00AAFF",
      "waypoints": [...]
    }
  ]
}
```

**Поля waypoint**:
- `x`, `y`, `z` — WoW coordinates (обязательные)
- `action` — опциональное: `"wait"`, `"interact"`, `"cast"` (для будущего использования)
- `delay` — опциональное: время ожидания в секундах

**Цвет** — hex строка #RRGGBB

#### `data/route_data.h/.cpp`

```cpp
struct RouteWaypoint {
    float x, y, z;
    std::string action;  // "", "wait", "interact", "cast"
    float delay = 0.0f;
};

struct Route {
    std::string name;
    uint32_t mapId = 0;
    bool loop = false;
    ImU32 color = IM_COL32(255, 200, 0, 255);
    std::vector<RouteWaypoint> waypoints;
};

class RouteData {
public:
    bool LoadFromFile(const std::string& path);
    bool SaveToFile(const std::string& path) const;

    std::vector<Route>& GetRoutes() { return m_routes; }
    const std::vector<Route>& GetRoutes() const { return m_routes; }

    int AddRoute(const Route& route);  // returns index
    void RemoveRoute(int index);

    bool IsDirty() const;
    void ClearDirty();

private:
    std::vector<Route> m_routes;
    bool m_dirty = false;
    std::string m_filePath;
};
```

**Парсинг цвета**:
```cpp
ImU32 ParseHexColor(const std::string& hex) {
    // "#RRGGBB" → IM_COL32(R, G, B, 255)
    unsigned int r, g, b;
    sscanf(hex.c_str() + 1, "%02x%02x%02x", &r, &g, &b);
    return IM_COL32(r, g, b, 255);
}

std::string ColorToHex(ImU32 col) {
    char buf[8];
    snprintf(buf, sizeof(buf), "#%02X%02X%02X",
        (col >> IM_COL32_R_SHIFT) & 0xFF,
        (col >> IM_COL32_G_SHIFT) & 0xFF,
        (col >> IM_COL32_B_SHIFT) & 0xFF);
    return buf;
}
```

#### `editor/route_editor.h/.cpp`

**UI**: Route List Panel (слева или как вкладка):
- Список маршрутов с цветными индикаторами
- Кнопки: New Route, Delete Route
- Для выбранного маршрута: Name (InputText), Map (dropdown), Loop (checkbox), Color (ColorEdit3)

**Editing mode** (когда маршрут выбран):
- Click на canvas → добавить waypoint в конец
- Click на существующий waypoint → выбрать его (Selection::Waypoint)
- Drag waypoint → переместить
- Delete → удалить выбранный waypoint
- Insert (клик между двумя waypoints) — опционально, можно через context menu

#### `render/route_renderer.h/.cpp`

Для каждого маршрута на текущей карте:
1. **Линии**: polyline через все waypoints цветом маршрута, толщина 2px
2. **Waypoints**: пронумерованные кружки (6px radius), заполненные цветом маршрута
3. **Стрелки направления**: маленькие стрелки на середине каждого сегмента
4. **Loop**: если loop=true, рисовать линию от последнего waypoint к первому (пунктиром)

```cpp
void RouteRenderer::Render(const Canvas& canvas, const RouteData& data, uint32_t mapId) {
    ImDrawList* dl = ImGui::GetWindowDrawList();

    for (const auto& route : data.GetRoutes()) {
        if (route.mapId != mapId) continue;
        if (route.waypoints.size() < 2) continue;

        // Draw segments
        for (size_t i = 0; i + 1 < route.waypoints.size(); ++i) {
            ImVec2 a = canvas.WorldToScreen(route.waypoints[i].x, route.waypoints[i].y);
            ImVec2 b = canvas.WorldToScreen(route.waypoints[i+1].x, route.waypoints[i+1].y);
            dl->AddLine(a, b, route.color, 2.0f);
        }

        // Loop closing line (dashed)
        if (route.loop) {
            // dashed line from last to first
        }

        // Draw waypoint circles with numbers
        for (size_t i = 0; i < route.waypoints.size(); ++i) {
            ImVec2 p = canvas.WorldToScreen(route.waypoints[i].x, route.waypoints[i].y);
            dl->AddCircleFilled(p, 6.0f, route.color);
            dl->AddCircle(p, 6.0f, IM_COL32(255, 255, 255, 200));

            char num[8];
            snprintf(num, sizeof(num), "%d", (int)(i + 1));
            // Center text in circle
            ImVec2 textSize = ImGui::CalcTextSize(num);
            dl->AddText(ImVec2(p.x - textSize.x*0.5f, p.y - textSize.y*0.5f),
                       IM_COL32(0, 0, 0, 255), num);
        }
    }
}
```

### Проверка Phase 4
Создать route "Test Route" → расставить 5 waypoints кликами по Elwynn Forest → включить Loop → Save routes.json → закрыть → переоткрыть → маршрут на месте с правильным цветом.

---

## Phase 5: Pathfinding Testing

### Цель
Тестирование Detour pathfinding прямо в редакторе — кликнуть две точки, увидеть путь.

### Файлы
`navmesh/pathfinder.h/.cpp`, `render/path_renderer.h/.cpp`

### Детали реализации

#### `navmesh/pathfinder.h/.cpp`

Упрощённая обёртка над dtNavMeshQuery (без danger zones, без DLL-специфики).

```cpp
struct PathResult {
    std::vector<std::pair<float,float>> waypoints;  // (wowX, wowY) pairs
    float totalDistance = 0.0f;
    bool success = false;
    bool partial = false;
};

class MapPathfinder {
public:
    // Uses TileCache's dtNavMesh and dtNavMeshQuery
    PathResult FindPath(dtNavMeshQuery* query,
                        float startX, float startY, float startZ,
                        float endX, float endY, float endZ);

private:
    static constexpr int kMaxPathPolygons = 256;
    static constexpr int kMaxStraightPath = 128;
    static constexpr float kSearchExtentXZ = 10.0f;
    static constexpr float kSearchExtentY = 20.0f;
    static constexpr float kLargeExtentXZ = 30.0f;
    static constexpr float kLargeExtentY = 40.0f;
};
```

**Реализация**: копия паттерна из `pathfinder.cpp:FindPath()`:
1. Convert WoW → Detour coords: `float startPos[3] = { startY, startZ, startX };`
2. `findNearestPoly` для start и end
3. `findPath` → poly path
4. `findStraightPath` → waypoints
5. Convert Detour → WoW: `wowX = straightPath[i*3+2], wowY = straightPath[i*3+0]`

**Загрузка тайлов**: перед pathfinding нужно убедиться что тайлы вокруг обеих точек загружены. Использовать TileCache для загрузки нужных тайлов (5x5 grid вокруг каждой точки).

#### `render/path_renderer.h/.cpp`

Рисует результат pathfinding:
- Яркая polyline (жёлтая #FFD700, толщина 3px)
- Точки A и B: большие кружки (красный = A, зелёный = B)
- Метка расстояния: "Distance: 342.5 yd (15 waypoints)"

**Test Path Mode** (toggle через UI):
1. Click → устанавливается точка A (рисуется красный кружок)
2. Click → устанавливается точка B → автоматический pathfinding → отображение результата
3. Click → reset, устанавливается новая точка A

```cpp
struct PathTestState {
    enum Mode { Idle, SetA, SetB };
    Mode mode = Idle;
    float startX, startY, startZ;
    float endX, endY, endZ;
    PathResult result;
    bool hasResult = false;
};
```

### Проверка Phase 5
Включить Test Path mode → кликнуть точку A в Goldshire → кликнуть точку B у стен Stormwind → увидеть жёлтый path по навмешу → показывается расстояние.

---

## Critical Constants & Formulas

### Tile Size
```cpp
static constexpr float TILE_SIZE = 533.33333f;  // 1 ADT = 533.33 yards
```

### WoW ↔ Tile Coordinate Mapping
```cpp
// World → Tile
tileX = 32 - (int)floor(wowX / TILE_SIZE);
tileY = 32 - (int)floor(wowY / TILE_SIZE);

// Tile → World bounds
wowX_min = (32 - tileX) * TILE_SIZE;
wowX_max = (33 - tileX) * TILE_SIZE;
wowY_min = (32 - tileY) * TILE_SIZE;
wowY_max = (33 - tileY) * TILE_SIZE;
```

### Detour ↔ WoW Coordinate Mapping
```cpp
// WoW → Detour
detour[0] = wowY;   // Detour X = WoW Y
detour[1] = wowZ;   // Detour Y = WoW Z (up)
detour[2] = wowX;   // Detour Z = WoW X

// Detour → WoW
wowX = detour[2];
wowY = detour[0];
wowZ = detour[1];
```

### WoW → Screen (North-Up Map)
```cpp
screenX = canvasCenterScreenX + (cameraWowY - wowY) * zoom;
screenY = canvasCenterScreenY + (wowX - cameraWowX) * zoom;
```

### 32-bit dtPolyRef Fix (для map editor с большим кэшем)
```cpp
params.maxTiles = 512;
// tileBits = ceil(log2(512)) = 9
// polyBits = 32 - 9 - 10(salt) = 13
// maxPolys = 2^13 = 8192
params.maxPolys = 8192;
```

### MmapTileHeader
```cpp
#pragma pack(push, 1)
struct MmapTileHeader {
    uint32_t mmapMagic;    // 0x4D4D4150
    uint32_t dtVersion;    // 7
    uint32_t mmapVersion;  // 15 or 16
    uint32_t size;         // payload size in bytes
    char     usesLiquids;
    char     padding[3];
};
#pragma pack(pop)
```

### Tile Data Repack (TC 16-byte → Stock 12-byte dtLink)
```cpp
const int kTcDtLinkSize = 16;
// preLinksSz = align4(sizeof(dtMeshHeader)) + align4(3*sizeof(float)*vertCount) + align4(sizeof(dtPoly)*polyCount)
// tcLinksSz = align4(kTcDtLinkSize * maxLinkCount)
// stockLinksSz = align4(sizeof(dtLink) * maxLinkCount)
// postLinksSz = align4(sizeof(dtPolyDetail)*detailMeshCount) + align4(3*sizeof(float)*detailVertCount)
//             + align4(4*detailTriCount) + align4(sizeof(dtBVNode)*bvNodeCount)
//             + align4(sizeof(dtOffMeshConnection)*offMeshConCount)
// Repack: copy pre-links, zero links, copy post-links
```

---

## Reference Files in Repository

| File | Purpose |
|------|---------|
| `wotlk/src/navigation/nav_mesh.h` | NavMesh class, MmapTileHeader, WorldToTile, kMaxTilesOverride |
| `wotlk/src/navigation/nav_mesh.cpp` | Full LoadMap + LoadTile + repack logic (~500 lines) |
| `wotlk/src/navigation/world_graph.h` | WorldNode, WorldEdge, NodeType, EdgeType enums |
| `wotlk/src/navigation/world_graph.cpp` | JSON parsing, A* route planning |
| `wotlk/src/navigation/pathfinder.h` | PathResult struct, search extents, Detour constants |
| `wotlk/src/navigation/pathfinder.cpp` | FindPath implementation, coord transforms |
| `wotlk/src/game/types.h` | Vec3 struct definition |
| `wotlk/src/overlay/overlay.cpp` | ImGui + ImDrawList rendering patterns, DX9 init (adapt to DX11) |
| `wotlk/src/bot/radar.cpp` | RadarData, WorldToRadar transform, circle/line drawing |
| `wotlk/docs/game_data/world_graph.json` | Actual graph data to load |
| `shared/imgui-dx9.props` | Template for imgui-dx11.props |
| `shared/detour-static.props` | Template for detour-x64.props |
| `shared/glog-common.props` | glog preprocessor defines, C++20, include path |
| `wotlk-utils.sln` | Solution file to add new project to |

---

## Build & Run

```bash
# 1. Install dependencies
vcpkg install nlohmann-json:x64-windows
vcpkg install recastnavigation:x64-windows

# 2. Open wotlk-utils.sln in Visual Studio 2022
# 3. Set map-editor as startup project
# 4. Select Debug|x64 configuration
# 5. Build and run (F5)

# mmaps directory path: hardcode or pass as command-line arg
# Default: "Z:/Games/wow 3.3.5a client/mmaps/" or make configurable
```

---

## Summary Checklist

- [ ] Phase 1: Skeleton App — DX11 window, canvas pan/zoom, tile grid, status bar, map switcher
- [ ] Phase 2: Navmesh Rendering — tile loader, LRU cache, triangle extraction, polygon rendering
- [ ] Phase 3: World Graph — load/save JSON, render nodes/edges, selection, property panel, CRUD editing
- [ ] Phase 4: Route Editor — load/save routes.json, waypoint placement, route list panel
- [ ] Phase 5: Pathfinding — Detour query wrapper, test path mode, path visualization
