# Build Instructions

## Requirements

- **Visual Studio 2022** with C++ Desktop workload
- **vcpkg** package manager
- **Platform**: x64 only (NOT x86/Win32)
- **C++ Standard**: C++20

## vcpkg Dependencies

```bash
vcpkg install imgui[dx11-binding,win32-binding]:x64-windows
vcpkg install glog:x64-windows
vcpkg install nlohmann-json:x64-windows
vcpkg install recastnavigation:x64-windows
vcpkg install stormlib:x64-windows
```

### Verify installation

```
C:\vcpkg\installed\x64-windows\include\imgui.h
C:\vcpkg\installed\x64-windows\include\imgui_impl_dx11.h
C:\vcpkg\installed\x64-windows\include\glog\logging.h
C:\vcpkg\installed\x64-windows\include\nlohmann\json.hpp
C:\vcpkg\installed\x64-windows\include\recastnavigation\DetourNavMesh.h
C:\vcpkg\installed\x64-windows\include\StormLib.h
```

## Project Configuration

### vcxproj Key Parameters

| Setting | Value |
|---------|-------|
| Platform | x64 |
| ConfigurationType | Application |
| SubSystem | Windows (WinMain) |
| LanguageStandard | stdcpp20 |
| RuntimeLibrary | MultiThreadedDLL (Release), MultiThreadedDebugDLL (Debug) |
| CharacterSet | Unicode |

### Preprocessor Definitions

```
GLOG_USE_GLOG_EXPORT
GLOG_NO_ABBREVIATED_SEVERITIES
WIN32_LEAN_AND_MEAN
NOMINMAX
```

### Props Files (in `shared/`)

| File | Purpose |
|------|---------|
| `glog-common.props` | glog includes, preprocessor, C++20 |
| `imgui-dx11.props` | ImGui + DX11 includes and libs |
| `detour-x64.props` | Detour navmesh libs (x64) |
| `nlohmann-json-x64.props` | Header-only JSON includes |
| `stormlib-x64.props` | StormLib MPQ reading libs |

### Post-Build: DLL Copying

The project automatically copies required DLLs (imgui, glog, gflags, StormLib) to the output directory.

## Build Steps

```bash
# Option 1: Visual Studio
# Open wotlk-utils.sln → Set map-editor as startup → Select Release|x64 → Build (F7)

# Option 2: MSBuild CLI
MSBuild map-editor/map-editor.vcxproj -p:Configuration=Release -p:Platform=x64
```

## Runtime Requirements

- WoW 3.3.5a client installation (for MPQ minimap tiles)
- TrinityCore `mmaps/` directory (for navmesh data)
- TrinityCore extracted data (for 3D terrain + buildings):
  - `maps/` — `.map` heightmap files (from `mapextractor`)
  - `vmaps/` — `.vmtile` spawn lists (from `vmap4assembler`)
  - `Buildings/` — extracted M2/WMO geometry (from `vmap4extractor`)
- `world_graph.json` (for POI graph data)

The TC data directories can be siblings of `mmaps/` or nested under `trinitycore_data/`:

```
wotlk/
    mmaps/                        ← set this as MMAP directory
    trinitycore_data/             ← auto-detected
        maps/                     ← .map heightmap files
        vmaps/                    ← .vmtile spawn lists
        Buildings/                ← M2/WMO model geometry
```

### Directory Layout at Runtime

```
map-editor.exe
imgui.dll
glog.dll
gflags.dll
StormLib.dll
logs/                   ← auto-created, glog output
app_settings.json       ← auto-created, persistent settings
```

## Solution Integration

The project is part of `wotlk-utils.sln`. Solution configurations:
- `Debug|x64` and `Release|x64` — map-editor builds
- `Debug|Win32` / `Release|Win32` — map-editor does NOT build (x86 configs are for wotlk DLL/injector only)

Project GUID: `{A1B2C3D4-E5F6-7890-ABCD-EF1234567890}`
