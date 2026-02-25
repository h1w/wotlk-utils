#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <cstdint>
#include <string>
#include <memory>

#include "canvas/canvas.h"
#include "data/map_defs.h"
#include "data/tile_index.h"
#include "data/world_graph_data.h"
#include "data/route_data.h"
#include "render/grid_renderer.h"
#include "render/navmesh_renderer.h"
#include "render/navmesh_renderer_3d.h"
#include "render/primitives_3d.h"
#include "render/graph_renderer_3d.h"
#include "render/route_renderer_3d.h"
#include "render/path_renderer_3d.h"
#include "render/grid_renderer_3d.h"
#include "render/ground_plane_3d.h"
#include "data/terrain_height_sampler.h"
#include "render/terrain_renderer.h"
#include "render/building_renderer.h"
#include "render/graph_renderer.h"
#include "render/route_renderer.h"
#include "render/path_renderer.h"
#include "render/map_background.h"
#include "render/minimap_cache.h"
#include "navmesh/tile_cache.h"
#include "navmesh/pathfinder.h"
#include "editor/selection.h"
#include "editor/graph_editor.h"
#include "editor/route_editor.h"
#include "editor/undo_redo.h"
#include "data/app_settings.h"
#include "ui/status_bar.h"
#include "ui/property_panel.h"
#include "ui/layer_panel.h"
#include "ui/main_menu.h"
#include "ui/log_window.h"
#include "ui/compass.h"
#include "render/frame_profiler.h"
#include "mpq/mpq_archive.h"
#include "mpq/world_map_loader.h"
#include "camera/camera3d.h"
#include "simulation/player_marker.h"

namespace mapedit {

enum class ViewMode { Mode2D, Mode3D };
enum class ActiveGraph { ReadOnly, World, Road };

class App {
public:
    bool Initialize(HINSTANCE hInstance);
    void Run();
    void Shutdown();

    static LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

private:
    bool CreateWindowAndDX11(HINSTANCE hInstance);
    void CleanupDX11();
    void CreateRenderTarget();
    void CleanupRenderTarget();
    void RenderFrame();
    void RenderFrame2D();
    void RenderFrame3D();
    void RenderMapSelector();
    void LoadSettings();
    void SaveSettings();
    bool HasUnsavedChanges() const;
    void SaveAllDirty();
    void RequestQuit();

    // Win32 / DX11
    HWND                    m_hwnd = nullptr;
    ID3D11Device*           m_device = nullptr;
    ID3D11DeviceContext*    m_context = nullptr;
    IDXGISwapChain*         m_swapChain = nullptr;
    ID3D11RenderTargetView* m_rtv = nullptr;
    ID3D11DepthStencilView* m_dsv = nullptr;
    ID3D11Texture2D*        m_depthTex = nullptr;
    UINT                    m_resizeWidth = 0;
    UINT                    m_resizeHeight = 0;

    // View mode
    ViewMode  m_viewMode = ViewMode::Mode2D;
    Camera3D  m_camera3d;

    // Player emulation (Phase 4)
    PlayerMarker m_playerMarker;

    // Core subsystems
    Canvas          m_canvas;
    TileIndex       m_tileIndex;
    TileCache       m_tileCache;
    MapPathfinder   m_pathfinder;

    // Renderers
    GridRenderer    m_gridRenderer;
    NavmeshRenderer   m_navmeshRenderer;
    NavmeshRenderer3D m_navmeshRenderer3d;
    Primitives3D      m_primitives3d;
    Graph3DRenderer   m_graphRenderer3d;
    Route3DRenderer   m_routeRenderer3d;
    Path3DRenderer    m_pathRenderer3d;
    Grid3DRenderer    m_gridRenderer3d;
    GroundPlane3D     m_groundPlane3d;
    TerrainRenderer   m_terrainRenderer;
    BuildingRenderer  m_buildingRenderer;
    GraphRenderer   m_graphRenderer;
    RouteRenderer   m_routeRenderer;
    PathRenderer    m_pathRenderer;
    MapBackground   m_mapBackground;
    MinimapTileCache m_minimapCache;

    // Height sampling
    TerrainHeightSampler m_heightSampler;

    // Editors
    GraphEditor     m_graphEditor;
    RouteEditor     m_routeEditor;
    MultiSelection  m_selection;
    UndoRedo        m_undoRedo;
    UndoRedo        m_roadUndoRedo;
    ActiveGraph     m_activeGraph = ActiveGraph::ReadOnly;

    // Data
    WorldGraphData  m_graphData;
    WorldGraphData  m_roadGraphData;
    RouteData       m_routeData;

    // UI
    StatusBar       m_statusBar;
    PropertyPanel   m_propertyPanel;
    LayerPanel      m_layerPanel;
    MainMenu        m_mainMenu;
    LayerVisibility m_layers;
    CompassWidget   m_compass;

    // Logging + Settings
    LogWindow      m_logWindow;
    AppSettings    m_settings;

    // MPQ / world map background
    MpqArchiveSet  m_mpq;
    WorldMapLoader m_worldMapLoader;

    // Performance diagnostics
    FrameProfiler m_profiler;
    bool          m_vsync = false;        // VSync OFF by default for diagnostics
    bool          m_showProfiler = true;  // Show profiler overlay
    int           m_fpsLimit = 0;         // 0 = unlimited, >0 = cap FPS
    LARGE_INTEGER m_frameStartQpc = {};   // For FPS limiter timing

    // State
    uint32_t     m_currentMapId = 0;
    uint32_t     m_lastTileCacheMapId = 0xFFFFFFFF;
    uint32_t     m_lastBgMapId = 0xFFFFFFFF;
    bool         m_mmapDirSet = false;
    std::string  m_mmapDir;
    bool         m_wowDirSet = false;
    MapBackgroundMode m_bgMode = MapBackgroundMode::MinimapTiles;
    uint32_t     m_lastMinimapMapId = 0xFFFFFFFF;
    bool         m_autoBgFromMpq = true;
    bool         m_showLegend = true;
    bool         m_showHelp = false;
    bool         m_showBgBoundsPopup = false;
    bool         m_showExitConfirm = false;
    std::string  m_pendingBgImagePath;
    float        m_bgBoundsMinX = 0, m_bgBoundsMaxX = 0;
    float        m_bgBoundsMinY = 0, m_bgBoundsMaxY = 0;
};

} // namespace mapedit
