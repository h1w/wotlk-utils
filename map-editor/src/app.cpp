#include "app.h"

#include <imgui.h>
#include <imgui_impl_win32.h>
#include <imgui_impl_dx11.h>
#include <glog/logging.h>

#include <d3d11.h>
#include <dxgi.h>
#include <dxgi1_4.h>
#include <combaseapi.h>
#include <psapi.h>

#pragma comment(lib, "psapi.lib")
#include <ShlObj.h>
#include <commdlg.h>
#include <algorithm>
#include <filesystem>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace mapedit {

static App* s_app = nullptr;

bool App::Initialize(HINSTANCE hInstance) {
    s_app = this;
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    // Initialize glog (no console — suppress stderr)
    google::InitGoogleLogging("map-editor");
    FLAGS_logtostderr = false;
    FLAGS_stderrthreshold = google::NUM_SEVERITIES;
    m_logWindow.Initialize("./logs");

    // Load saved settings
    LoadSettings();

    if (!CreateWindowAndDX11(hInstance))
        return false;

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    ImGui::StyleColorsDark();

    ImGui_ImplWin32_Init(m_hwnd);
    ImGui_ImplDX11_Init(m_device, m_context);

    m_navmeshRenderer.Initialize(m_device, m_context);
    m_navmeshRenderer3d.Initialize(m_device, m_context);
    m_primitives3d.Initialize(m_device, m_context);
    m_groundPlane3d.Initialize(m_device, m_context);
    m_terrainRenderer.Initialize(m_device, m_context);
    m_buildingRenderer.Initialize(m_device, m_context);

    // Restore saved state
    if (!m_settings.mmapDir.empty()) {
        m_mmapDir = m_settings.mmapDir;
        m_tileIndex.ScanDirectory(m_mmapDir);
        m_mmapDirSet = true;
        m_lastTileCacheMapId = 0xFFFFFFFF;
        // Terrain/building data path = parent of mmaps dir, auto-detect layout
        {
            namespace fs = std::filesystem;
            fs::path mmapPath(m_mmapDir);
            if (mmapPath.filename().empty()) // strip trailing separator
                mmapPath = mmapPath.parent_path();
            fs::path parent = mmapPath.parent_path();
            std::string tcDataPath;
            if (fs::exists(parent / "maps"))
                tcDataPath = parent.string();
            else if (fs::exists(parent / "trinitycore_data" / "maps"))
                tcDataPath = (parent / "trinitycore_data").string();
            if (!tcDataPath.empty()) {
                m_terrainRenderer.SetDataPath(tcDataPath);
                m_buildingRenderer.SetDataPath(tcDataPath);
                LOG(INFO) << "[App] TC data path: " << tcDataPath;
            }
        }
        if (m_settings.lastMapId != 0)
            m_currentMapId = m_settings.lastMapId;
        else {
            auto mapIds = m_tileIndex.GetMapIds();
            if (!mapIds.empty()) m_currentMapId = mapIds[0];
        }
    }
    if (!m_settings.wowDir.empty()) {
        if (m_mpq.Open(m_settings.wowDir)) {
            m_worldMapLoader.Initialize(m_mpq);
            m_minimapCache.Initialize(m_mpq);
            m_wowDirSet = true;
            m_lastBgMapId = 0xFFFFFFFF;
            m_lastMinimapMapId = 0xFFFFFFFF;
        }
    }
    if (!m_settings.worldGraphPath.empty()) {
        m_graphData.LoadFromFile(m_settings.worldGraphPath);
        m_graphData.ClearDirty();
    }
    if (!m_settings.routesPath.empty()) {
        m_routeData.LoadFromFile(m_settings.routesPath);
        m_routeData.ClearDirty();
    }

    m_autoBgFromMpq = m_settings.autoBgFromMpq;
    m_showLegend = m_settings.showLegend;
    m_mapBackground.SetOpacity(m_settings.bgOpacity);
    m_bgMode = static_cast<MapBackgroundMode>(std::clamp(m_settings.bgMode, 0, 3));

    // Restore layer visibility
    m_layers.showGrid = m_settings.showGrid;
    m_layers.showNavmesh = m_settings.showNavmesh;
    m_layers.showNodes = m_settings.showNodes;
    m_layers.showEdges = m_settings.showEdges;
    m_layers.showLabels = m_settings.showLabels;
    m_layers.showRoutes = m_settings.showRoutes;
    m_layers.showPath = m_settings.showPath;
    m_layers.showBackground = m_settings.showBackground;
    m_layers.navmeshMinZoom = m_settings.navmeshMinZoom;
    m_layers.navmeshMaxTiles = m_settings.navmeshMaxTiles;
    m_layers.showTerrain = m_settings.showTerrain;
    m_layers.terrainColorMode = m_settings.terrainColorMode;
    m_layers.showBuildings = m_settings.showBuildings;
    m_layers.showBuildingObjects = m_settings.showBuildingObjects;
    m_layers.enablePortalCulling = m_settings.enablePortalCulling;

    LOG(INFO) << "[App] Initialized";
    return true;
}

bool App::CreateWindowAndDX11(HINSTANCE hInstance) {
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.style = CS_CLASSDC;
    wc.lpfnWndProc = App::WndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = L"MapEditorClass";
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    RegisterClassExW(&wc);

    int winW = (m_settings.windowWidth > 0) ? m_settings.windowWidth : 1600;
    int winH = (m_settings.windowHeight > 0) ? m_settings.windowHeight : 1000;
    m_hwnd = CreateWindowW(L"MapEditorClass", L"WoW Map Editor",
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
        winW, winH, nullptr, nullptr, hInstance, nullptr);

    if (!m_hwnd) {
        LOG(ERROR) << "[App] CreateWindow failed";
        return false;
    }

    DXGI_SWAP_CHAIN_DESC sd = {};
    sd.BufferCount = 2;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = m_hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    UINT createFlags = 0;
#ifdef _DEBUG
    createFlags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

    D3D_FEATURE_LEVEL featureLevel;
    HRESULT hr = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, createFlags,
        nullptr, 0, D3D11_SDK_VERSION, &sd,
        &m_swapChain, &m_device, &featureLevel, &m_context);

    if (FAILED(hr)) {
        LOG(ERROR) << "[App] D3D11CreateDeviceAndSwapChain failed: 0x" << std::hex << hr;
        return false;
    }

    CreateRenderTarget();
    ShowWindow(m_hwnd, SW_SHOWDEFAULT);
    UpdateWindow(m_hwnd);

    LOG(INFO) << "[App] DX11 initialized, feature level: 0x" << std::hex << featureLevel;
    return true;
}

void App::CreateRenderTarget() {
    ID3D11Texture2D* backBuffer = nullptr;
    m_swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer));
    if (backBuffer) {
        m_device->CreateRenderTargetView(backBuffer, nullptr, &m_rtv);

        // Create depth-stencil buffer (used by 3D mode only)
        D3D11_TEXTURE2D_DESC bbDesc = {};
        backBuffer->GetDesc(&bbDesc);

        D3D11_TEXTURE2D_DESC dd = {};
        dd.Width     = bbDesc.Width;
        dd.Height    = bbDesc.Height;
        dd.MipLevels = 1;
        dd.ArraySize = 1;
        dd.Format    = DXGI_FORMAT_D24_UNORM_S8_UINT;
        dd.SampleDesc.Count = 1;
        dd.Usage     = D3D11_USAGE_DEFAULT;
        dd.BindFlags = D3D11_BIND_DEPTH_STENCIL;
        m_device->CreateTexture2D(&dd, nullptr, &m_depthTex);
        if (m_depthTex)
            m_device->CreateDepthStencilView(m_depthTex, nullptr, &m_dsv);

        backBuffer->Release();
    }
}

void App::CleanupRenderTarget() {
    if (m_dsv)      { m_dsv->Release();      m_dsv = nullptr; }
    if (m_depthTex) { m_depthTex->Release();  m_depthTex = nullptr; }
    if (m_rtv)      { m_rtv->Release();       m_rtv = nullptr; }
}

void App::CleanupDX11() {
    CleanupRenderTarget();
    if (m_swapChain) { m_swapChain->Release(); m_swapChain = nullptr; }
    if (m_context)   { m_context->Release();   m_context = nullptr; }
    if (m_device)    { m_device->Release();     m_device = nullptr; }
}

void App::Run() {
    MSG msg = {};
    while (msg.message != WM_QUIT) {
        while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
            if (msg.message == WM_QUIT)
                return;
        }

        if (m_resizeWidth != 0 && m_resizeHeight != 0) {
            CleanupRenderTarget();
            m_swapChain->ResizeBuffers(0, m_resizeWidth, m_resizeHeight,
                                        DXGI_FORMAT_UNKNOWN, 0);
            m_resizeWidth = m_resizeHeight = 0;
            CreateRenderTarget();
        }

        RenderFrame();
    }
}

void App::RenderFrame() {
    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();

    // === Main Menu Bar ===
    if (ImGui::BeginMainMenuBar()) {
        if (ImGui::BeginMenu("File")) {
            if (ImGui::MenuItem("Open MMAP Directory...")) {
                IFileDialog* pfd = nullptr;
                HRESULT hr = CoCreateInstance(CLSID_FileOpenDialog, nullptr,
                    CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&pfd));
                if (SUCCEEDED(hr)) {
                    DWORD options = 0;
                    pfd->GetOptions(&options);
                    pfd->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
                    pfd->SetTitle(L"Select MMAP Directory");
                    if (SUCCEEDED(pfd->Show(m_hwnd))) {
                        IShellItem* psi = nullptr;
                        if (SUCCEEDED(pfd->GetResult(&psi))) {
                            PWSTR wpath = nullptr;
                            if (SUCCEEDED(psi->GetDisplayName(SIGDN_FILESYSPATH, &wpath))) {
                                char narrow[MAX_PATH];
                                WideCharToMultiByte(CP_UTF8, 0, wpath, -1,
                                                    narrow, MAX_PATH, nullptr, nullptr);
                                m_mmapDir = narrow;
                                if (!m_mmapDir.empty() && m_mmapDir.back() != '\\' &&
                                    m_mmapDir.back() != '/')
                                    m_mmapDir += '\\';
                                m_tileIndex.ScanDirectory(m_mmapDir);
                                m_mmapDirSet = true;
                                m_lastTileCacheMapId = 0xFFFFFFFF;
                                // Terrain/building data path = parent of mmaps dir, auto-detect layout
                                {
                                    namespace fs = std::filesystem;
                                    fs::path mmapPath(m_mmapDir);
                                    if (mmapPath.filename().empty())
                                        mmapPath = mmapPath.parent_path();
                                    fs::path parent = mmapPath.parent_path();
                                    std::string tcDataPath;
                                    if (fs::exists(parent / "maps"))
                                        tcDataPath = parent.string();
                                    else if (fs::exists(parent / "trinitycore_data" / "maps"))
                                        tcDataPath = (parent / "trinitycore_data").string();
                                    if (!tcDataPath.empty()) {
                                        m_terrainRenderer.SetDataPath(tcDataPath);
                                        m_buildingRenderer.SetDataPath(tcDataPath);
                                        LOG(INFO) << "[App] TC data path: " << tcDataPath;
                                    }
                                }
                                auto mapIds = m_tileIndex.GetMapIds();
                                if (!mapIds.empty())
                                    m_currentMapId = mapIds[0];
                                CoTaskMemFree(wpath);
                            }
                            psi->Release();
                        }
                    }
                    pfd->Release();
                }
            }
            if (ImGui::MenuItem("Set WoW Directory...")) {
                IFileDialog* pfd = nullptr;
                HRESULT hr = CoCreateInstance(CLSID_FileOpenDialog, nullptr,
                    CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&pfd));
                if (SUCCEEDED(hr)) {
                    DWORD options = 0;
                    pfd->GetOptions(&options);
                    pfd->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
                    pfd->SetTitle(L"Select WoW Data Directory");
                    if (SUCCEEDED(pfd->Show(m_hwnd))) {
                        IShellItem* psi = nullptr;
                        if (SUCCEEDED(pfd->GetResult(&psi))) {
                            PWSTR wpath = nullptr;
                            if (SUCCEEDED(psi->GetDisplayName(SIGDN_FILESYSPATH, &wpath))) {
                                char narrow[MAX_PATH];
                                WideCharToMultiByte(CP_UTF8, 0, wpath, -1,
                                                    narrow, MAX_PATH, nullptr, nullptr);
                                if (m_mpq.Open(narrow)) {
                                    m_worldMapLoader.Initialize(m_mpq);
                                    m_minimapCache.Initialize(m_mpq);
                                    m_wowDirSet = true;
                                    m_lastBgMapId = 0xFFFFFFFF;
                                    m_lastMinimapMapId = 0xFFFFFFFF;
                                    m_settings.wowDir = narrow;
                                }
                                CoTaskMemFree(wpath);
                            }
                            psi->Release();
                        }
                    }
                    pfd->Release();
                }
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Exit"))
                PostQuitMessage(0);
            ImGui::EndMenu();
        }

        // Edit menu (Undo/Redo)
        if (ImGui::BeginMenu("Edit")) {
            char undoLabel[128];
            snprintf(undoLabel, sizeof(undoLabel), "Undo %s", m_undoRedo.UndoDescription());
            if (ImGui::MenuItem(undoLabel, "Ctrl+Z", false, m_undoRedo.CanUndo()))
                m_undoRedo.Undo(m_graphData, m_routeData);

            char redoLabel[128];
            snprintf(redoLabel, sizeof(redoLabel), "Redo %s", m_undoRedo.RedoDescription());
            if (ImGui::MenuItem(redoLabel, "Ctrl+Shift+Z", false, m_undoRedo.CanRedo()))
                m_undoRedo.Redo(m_graphData, m_routeData);

            ImGui::Separator();
            ImGui::TextDisabled("Undo: %zu / %zu", m_undoRedo.UndoCount(), UndoRedo::kMaxDepth);
            ImGui::EndMenu();
        }

        // Graph menu (Phase 3)
        auto menuActions = m_mainMenu.Render(m_graphData);
        if (menuActions.openGraph) {
            m_graphData.LoadFromFile(menuActions.graphFilePath);
            m_undoRedo.Clear();
        }
        if (menuActions.saveGraph && m_graphData.IsLoaded()) {
            m_graphData.SaveToFile(m_graphData.GetFilePath());
            m_graphData.ClearDirty();
        }
        if (menuActions.saveGraphAs) {
            m_graphData.SaveToFile(menuActions.graphFilePath);
            m_graphData.ClearDirty();
        }

        // Route menu
        if (ImGui::BeginMenu("Routes")) {
            if (ImGui::MenuItem("Open Routes JSON...")) {
                char filename[MAX_PATH] = {};
                OPENFILENAMEA ofn = {};
                ofn.lStructSize = sizeof(ofn);
                ofn.hwndOwner = m_hwnd;
                ofn.lpstrFilter = "JSON files\0*.json\0All files\0*.*\0";
                ofn.lpstrFile = filename;
                ofn.nMaxFile = MAX_PATH;
                ofn.lpstrTitle = "Open Routes";
                ofn.Flags = OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;
                if (GetOpenFileNameA(&ofn)) {
                    m_routeData.LoadFromFile(filename);
                    m_undoRedo.Clear();
                }
            }
            if (ImGui::MenuItem("Save Routes", nullptr, false, m_routeData.IsLoaded())) {
                m_routeData.SaveToFile(m_routeData.GetFilePath());
                m_routeData.ClearDirty();
            }
            if (ImGui::MenuItem("Save Routes As...")) {
                char filename[MAX_PATH] = {};
                OPENFILENAMEA ofn = {};
                ofn.lStructSize = sizeof(ofn);
                ofn.hwndOwner = m_hwnd;
                ofn.lpstrFilter = "JSON files\0*.json\0All files\0*.*\0";
                ofn.lpstrFile = filename;
                ofn.nMaxFile = MAX_PATH;
                ofn.lpstrTitle = "Save Routes";
                ofn.Flags = OFN_OVERWRITEPROMPT | OFN_NOCHANGEDIR;
                if (GetSaveFileNameA(&ofn)) {
                    m_routeData.SaveToFile(filename);
                    m_routeData.ClearDirty();
                }
            }
            ImGui::EndMenu();
        }

        // Tools menu (path testing)
        if (ImGui::BeginMenu("Tools")) {
            if (ImGui::MenuItem("Test Path (click A, B)", nullptr,
                                m_pathRenderer.GetState() != PathTestState::Idle)) {
                if (m_pathRenderer.GetState() == PathTestState::Idle)
                    m_pathRenderer.SetState(PathTestState::SetA);
                else
                    m_pathRenderer.Reset();
            }
            if (ImGui::MenuItem("Reset Path", nullptr, false, m_pathRenderer.HasResult())) {
                m_pathRenderer.Reset();
            }
            ImGui::EndMenu();
        }

        // View menu
        if (ImGui::BeginMenu("View")) {
            if (ImGui::MenuItem("Mode: 2D", "Ctrl+1", m_viewMode == ViewMode::Mode2D))
                m_viewMode = ViewMode::Mode2D;
            if (ImGui::MenuItem("Mode: 3D", "Ctrl+2", m_viewMode == ViewMode::Mode3D))
                m_viewMode = ViewMode::Mode3D;
            ImGui::Separator();
            ImGui::MenuItem("Grid", nullptr, &m_layers.showGrid);
            ImGui::MenuItem("Navmesh", nullptr, &m_layers.showNavmesh);
            ImGui::MenuItem("Nodes", nullptr, &m_layers.showNodes);
            ImGui::MenuItem("Edges", nullptr, &m_layers.showEdges);
            ImGui::MenuItem("Labels", nullptr, &m_layers.showLabels);
            ImGui::MenuItem("Routes", nullptr, &m_layers.showRoutes);
            ImGui::MenuItem("Path Test", nullptr, &m_layers.showPath);
            ImGui::MenuItem("Legend", nullptr, &m_showLegend);
            {
                bool logOpen = m_logWindow.IsOpen();
                if (ImGui::MenuItem("Log", nullptr, &logOpen))
                    m_logWindow.SetOpen(logOpen);
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Load Map Background...")) {
                char filename[MAX_PATH] = {};
                OPENFILENAMEA ofn = {};
                ofn.lStructSize = sizeof(ofn);
                ofn.hwndOwner = m_hwnd;
                ofn.lpstrFilter = "Images\0*.png;*.bmp;*.jpg;*.jpeg;*.tga\0All files\0*.*\0";
                ofn.lpstrFile = filename;
                ofn.nMaxFile = MAX_PATH;
                ofn.lpstrTitle = "Load Map Background Image";
                ofn.Flags = OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;
                if (GetOpenFileNameA(&ofn)) {
                    m_pendingBgImagePath = filename;
                    // Compute default bounds from tile extents
                    const auto& tiles = m_tileIndex.GetTilesForMap(m_currentMapId);
                    if (!tiles.empty()) {
                        int tMinX = 64, tMaxX = 0, tMinY = 64, tMaxY = 0;
                        for (const auto& t : tiles) {
                            if (t.x < tMinX) tMinX = t.x;
                            if (t.x > tMaxX) tMaxX = t.x;
                            if (t.y < tMinY) tMinY = t.y;
                            if (t.y > tMaxY) tMaxY = t.y;
                        }
                        constexpr float kTS = 533.33333f;
                        m_bgBoundsMinX = (31 - tMaxX) * kTS;
                        m_bgBoundsMaxX = (32 - tMinX) * kTS;
                        m_bgBoundsMinY = (31 - tMaxY) * kTS;
                        m_bgBoundsMaxY = (32 - tMinY) * kTS;
                    } else {
                        m_bgBoundsMinX = -10000; m_bgBoundsMaxX = 10000;
                        m_bgBoundsMinY = -10000; m_bgBoundsMaxY = 10000;
                    }
                    m_showBgBoundsPopup = true;
                }
            }
            ImGui::MenuItem("Auto Background (MPQ)", nullptr, &m_autoBgFromMpq);
            if (m_mapBackground.IsLoaded()) {
                float opacity = m_mapBackground.GetOpacity();
                ImGui::SetNextItemWidth(120);
                if (ImGui::SliderFloat("BG Opacity", &opacity, 0.0f, 1.0f, "%.2f"))
                    m_mapBackground.SetOpacity(opacity);
                if (ImGui::MenuItem("Remove Background")) {
                    m_mapBackground.Release();
                    m_lastBgMapId = 0xFFFFFFFF;
                }
            }
            if (!m_wowDirSet) {
                ImGui::Separator();
                ImGui::TextDisabled("Set WoW Directory for auto backgrounds");
            }
            ImGui::EndMenu();
        }

        ImGui::EndMainMenuBar();
    }

    // === Toolbar ===
    RenderMapSelector();

    // === Canvas viewport ===
    ImGuiViewport* vp = ImGui::GetMainViewport();
    float toolbarH = 36.0f;
    float statusBarH = 28.0f;
    float propertyPanelW = 280.0f;
    m_canvas.vpX = vp->WorkPos.x;
    m_canvas.vpY = vp->WorkPos.y + toolbarH;
    m_canvas.vpW = vp->WorkSize.x - propertyPanelW;
    m_canvas.vpH = vp->WorkSize.y - toolbarH - statusBarH;

    // === Update tile cache when map changes ===
    if (m_mmapDirSet && m_currentMapId != m_lastTileCacheMapId) {
        m_tileCache.SetMap(m_mmapDir, m_currentMapId);
        m_lastTileCacheMapId = m_currentMapId;
    }

    // === Auto-load world map background from MPQ ===
    if (m_autoBgFromMpq && m_wowDirSet && m_bgMode == MapBackgroundMode::ZoneWorldMaps
        && m_currentMapId != m_lastBgMapId) {
        m_lastBgMapId = m_currentMapId;
        float prevOpacity = m_mapBackground.GetOpacity();
        if (m_worldMapLoader.HasMap(m_currentMapId)) {
            m_worldMapLoader.LoadMap(m_mpq, m_device, m_currentMapId, m_mapBackground);
            m_mapBackground.SetOpacity(prevOpacity);
        } else {
            m_mapBackground.Release();
        }
    }

    // === Minimap tile cache: map change + viewport streaming ===
    {
        bool needMinimapUpdate =
            (m_bgMode == MapBackgroundMode::MinimapTiles && m_layers.showBackground) ||
            (m_viewMode == ViewMode::Mode3D && m_layers.showGroundPlane);

        if (m_wowDirSet && needMinimapUpdate) {
            if (m_currentMapId != m_lastMinimapMapId) {
                m_minimapCache.SetMap(m_currentMapId);
                m_lastMinimapMapId = m_currentMapId;
            }
            m_minimapCache.UpdateViewport(m_canvas, m_mpq, m_device);
        }
    }

    // === Keyboard shortcuts: Undo/Redo ===
    if (!ImGui::GetIO().WantTextInput) {
        if (ImGui::GetIO().KeyCtrl && ImGui::GetIO().KeyShift &&
            ImGui::IsKeyPressed(ImGuiKey_Z)) {
            m_undoRedo.Redo(m_graphData, m_routeData);
        } else if (ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Z)) {
            m_undoRedo.Undo(m_graphData, m_routeData);
        }
    }

    // === View mode toggle shortcuts ===
    if (!ImGui::GetIO().WantTextInput) {
        if (ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_1))
            m_viewMode = ViewMode::Mode2D;
        if (ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_2))
            m_viewMode = ViewMode::Mode3D;

        // 3D mode shortcuts
        if (m_viewMode == ViewMode::Mode3D) {
            if (ImGui::IsKeyPressed(ImGuiKey_G))
                m_layers.showGrid = !m_layers.showGrid;
            if (ImGui::IsKeyPressed(ImGuiKey_N))
                m_layers.showNavmesh = !m_layers.showNavmesh;
            if (ImGui::IsKeyPressed(ImGuiKey_C))
                m_camera3d.followMode = !m_camera3d.followMode;
            if (ImGui::IsKeyPressed(ImGuiKey_Space) && m_playerMarker.placed) {
                if (m_playerMarker.isMoving)
                    m_playerMarker.Stop();
            }
            if (ImGui::IsKeyPressed(ImGuiKey_Equal) || ImGui::IsKeyPressed(ImGuiKey_KeypadAdd)) {
                if (m_playerMarker.speedMultiplier < 10.0f)
                    m_playerMarker.speedMultiplier = (std::min)(m_playerMarker.speedMultiplier * 2.0f, 10.0f);
            }
            if (ImGui::IsKeyPressed(ImGuiKey_Minus) || ImGui::IsKeyPressed(ImGuiKey_KeypadSubtract)) {
                if (m_playerMarker.speedMultiplier > 1.0f)
                    m_playerMarker.speedMultiplier = (std::max)(m_playerMarker.speedMultiplier * 0.5f, 1.0f);
            }
            // F = focus on selection
            if (ImGui::IsKeyPressed(ImGuiKey_F)) {
                if (m_playerMarker.placed) {
                    m_camera3d.targetX = m_playerMarker.posX;
                    m_camera3d.targetY = m_playerMarker.posY;
                    m_camera3d.targetZ = m_playerMarker.posZ + 2.0f;
                } else if (m_selection.IsNode()) {
                    const auto* node = m_graphData.GetNode(m_selection.nodeId);
                    if (node) {
                        m_camera3d.targetX = node->x;
                        m_camera3d.targetY = node->y;
                        m_camera3d.targetZ = node->z + 2.0f;
                    }
                }
            }
        }
    }

    // === Mode-specific rendering ===
    if (m_viewMode == ViewMode::Mode2D) {
        RenderFrame2D();
    } else {
        RenderFrame3D();
    }

    // === Ctrl+S shortcut ===
    if (ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_S)) {
        if (m_graphData.IsLoaded() && m_graphData.IsDirty()) {
            m_graphData.SaveToFile(m_graphData.GetFilePath());
            m_graphData.ClearDirty();
        }
        if (m_routeData.IsLoaded() && m_routeData.IsDirty()) {
            m_routeData.SaveToFile(m_routeData.GetFilePath());
            m_routeData.ClearDirty();
        }
    }

    // === DX11 render ===
    const float clear_color[4] = { 0.1f, 0.1f, 0.12f, 1.0f };

    if (m_viewMode == ViewMode::Mode3D) {
        // 3D mode: clear backbuffer + depth FIRST, then 3D scene renders
        // (already rendered by NavmeshRenderer3D in RenderFrame3D above),
        // then ImGui as overlay on top.
        // NOTE: NavmeshRenderer3D::Render() clears depth and renders to RTV+DSV
        // before this point. We just need to clear the backbuffer before that.
        // Since the 3D renderer already ran, we just need ImGui overlay.
        ImGui::Render();
        m_context->OMSetRenderTargets(1, &m_rtv, nullptr);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    } else {
        // 2D mode: original flow — clear + ImGui (which includes navmesh via callback)
        ImGui::Render();
        m_context->OMSetRenderTargets(1, &m_rtv, nullptr);
        m_context->ClearRenderTargetView(m_rtv, clear_color);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    }

    m_swapChain->Present(1, 0);
}

// ---------------------------------------------------------------------------
// RenderFrame2D — all existing 2D logic (unchanged from original)
// ---------------------------------------------------------------------------

void App::RenderFrame2D() {
    // === Process input (editors consume first, then canvas) ===
    UndoContext undo{m_undoRedo, m_graphData, m_routeData};

    bool inputConsumed = false;

    // Path test input
    if (m_layers.showPath && m_pathRenderer.GetState() != PathTestState::Idle) {
        inputConsumed = m_pathRenderer.ProcessInput(m_canvas, m_tileCache, m_pathfinder);
    }

    // Route editor input
    if (!inputConsumed && m_layers.showRoutes && m_routeEditor.IsEditing()) {
        inputConsumed = m_routeEditor.ProcessInput(m_canvas, m_routeData, m_currentMapId, undo);
    }

    // Graph editor input
    if (!inputConsumed && m_graphData.IsLoaded() && m_layers.showNodes) {
        inputConsumed = m_graphEditor.ProcessInput(m_canvas, m_graphData, m_selection, m_currentMapId, undo);
    }

    // Canvas pan/zoom (only if no editor consumed the input)
    if (!inputConsumed) {
        m_canvas.ProcessInput();
    } else {
        // Still need to handle panning if already started
        if (m_canvas.panning)
            m_canvas.ProcessInput();
    }

    // === Update tile cache viewport (zoom gates rendering) ===
    if (m_layers.showNavmesh && m_tileCache.GetNavMesh() && m_canvas.zoom >= m_layers.navmeshMinZoom) {
        m_tileCache.SetMaxTiles(m_layers.navmeshMaxTiles);
        m_tileCache.UpdateViewport(m_canvas);
    }

    // === Render layers (back to front) ===
    // Map background (very back)
    if (m_layers.showBackground) {
        if (m_bgMode == MapBackgroundMode::MinimapTiles)
            m_minimapCache.Render(m_canvas);
        else if (m_bgMode == MapBackgroundMode::ZoneWorldMaps)
            m_mapBackground.Render(m_canvas);
    }

    // Grid (background)
    if (m_layers.showGrid)
        m_gridRenderer.Render(m_canvas, m_tileIndex, m_currentMapId);

    // Navmesh
    if (m_layers.showNavmesh)
        m_navmeshRenderer.Render(m_canvas, m_tileCache, m_layers.navmeshMinZoom);

    // Graph (nodes + edges)
    if (m_graphData.IsLoaded())
        m_graphRenderer.Render(m_canvas, m_graphData, m_currentMapId, m_selection, m_layers);

    // Routes
    if (m_layers.showRoutes)
        m_routeRenderer.Render(m_canvas, m_routeData, m_routeEditor, m_currentMapId);

    // Path test
    if (m_layers.showPath)
        m_pathRenderer.Render(m_canvas);

    // === UI Panels ===
    m_propertyPanel.Render(m_graphData, m_selection, m_currentMapId,
                           m_canvas, m_tileCache, undo);

    m_routeEditor.RenderPanel(m_routeData, m_currentMapId, undo);
    m_layerPanel.Render(m_layers, m_bgMode, &m_minimapCache);
    m_logWindow.Render();

    // === Legend panel (bottom-right) ===
    if (m_showLegend) {
        ImGuiViewport* lvp = ImGui::GetMainViewport();
        float propW = 280.0f;
        ImGui::SetNextWindowPos(
            ImVec2(lvp->WorkPos.x + lvp->WorkSize.x - propW - 10,
                   lvp->WorkPos.y + lvp->WorkSize.y - 28.0f - 10),
            ImGuiCond_Always, ImVec2(1.0f, 1.0f));
        ImGui::SetNextWindowBgAlpha(0.75f);
        if (ImGui::Begin("Legend", &m_showLegend,
                ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
                ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoFocusOnAppearing |
                ImGuiWindowFlags_NoNav)) {
            auto legendItem = [](ImVec4 color, const char* label) {
                ImGui::ColorButton(label, color,
                    ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoPicker |
                    ImGuiColorEditFlags_NoBorder, ImVec2(12, 12));
                ImGui::SameLine();
                ImGui::TextUnformatted(label);
            };

            ImGui::SeparatorText("Layers");
            legendItem(ImVec4(0.0f, 0.71f, 0.31f, 0.16f), "Navmesh");

            ImGui::SeparatorText("Nodes");
            legendItem(ImVec4(1.0f, 0.78f, 0.0f, 1.0f), "Flight Master");
            legendItem(ImVec4(0.50f, 0.0f, 1.0f, 1.0f), "Portal");
            legendItem(ImVec4(0.0f, 0.59f, 1.0f, 1.0f), "Boat / Zeppelin");
            legendItem(ImVec4(0.0f, 0.78f, 0.39f, 1.0f), "Innkeeper");
            legendItem(ImVec4(0.78f, 0.78f, 0.78f, 1.0f), "Zone Boundary");
            legendItem(ImVec4(0.78f, 0.20f, 0.20f, 1.0f), "Dungeon Entrance");
            legendItem(ImVec4(0.71f, 0.71f, 0.71f, 1.0f), "Waypoint");

            ImGui::SeparatorText("Edges");
            legendItem(ImVec4(0.59f, 0.59f, 0.59f, 0.39f), "Walk");
            legendItem(ImVec4(1.0f, 0.78f, 0.0f, 0.59f), "Flight");
            legendItem(ImVec4(0.50f, 0.0f, 1.0f, 0.59f), "Teleport");
            legendItem(ImVec4(0.0f, 0.59f, 1.0f, 0.59f), "Boat");

            ImGui::SeparatorText("Path Test");
            legendItem(ImVec4(1.0f, 0.20f, 0.20f, 0.86f), "Point A");
            legendItem(ImVec4(0.20f, 1.0f, 0.20f, 0.86f), "Point B");
            legendItem(ImVec4(1.0f, 1.0f, 0.0f, 0.86f), "Path");
            legendItem(ImVec4(1.0f, 0.65f, 0.0f, 0.86f), "Path (partial)");
        }
        ImGui::End();
    }

    // === Background bounds popup ===
    if (m_showBgBoundsPopup) {
        ImGui::OpenPopup("Map Background Bounds");
        m_showBgBoundsPopup = false;
    }
    if (ImGui::BeginPopupModal("Map Background Bounds", nullptr,
            ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Set world coordinate bounds for the background image:");
        ImGui::Separator();
        ImGui::InputFloat("Min X (north)", &m_bgBoundsMinX, 100.0f, 1000.0f, "%.1f");
        ImGui::InputFloat("Max X (south)", &m_bgBoundsMaxX, 100.0f, 1000.0f, "%.1f");
        ImGui::InputFloat("Min Y (east)",  &m_bgBoundsMinY, 100.0f, 1000.0f, "%.1f");
        ImGui::InputFloat("Max Y (west)",  &m_bgBoundsMaxY, 100.0f, 1000.0f, "%.1f");
        ImGui::Separator();
        if (ImGui::Button("OK", ImVec2(120, 0))) {
            m_mapBackground.LoadFromFile(m_device, m_pendingBgImagePath,
                m_bgBoundsMinX, m_bgBoundsMaxX,
                m_bgBoundsMinY, m_bgBoundsMaxY);
            m_pendingBgImagePath.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(120, 0))) {
            m_pendingBgImagePath.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    // === Compass (2D: fixed south-up) ===
    m_compass.Render(m_canvas.vpX, m_canvas.vpY, m_canvas.vpW, m_canvas.vpH, 0.0f);

    // === Status bar ===
    const char* mapName = "No Map";
    if (auto* mi = FindMap(m_currentMapId))
        mapName = mi->name;
    m_statusBar.Render(m_canvas, m_currentMapId, mapName);
}

// ---------------------------------------------------------------------------
// RenderFrame3D — 3D navmesh viewing mode
// ---------------------------------------------------------------------------

void App::RenderFrame3D() {
    // Set 3D camera viewport to match canvas area
    m_camera3d.vpX = m_canvas.vpX;
    m_camera3d.vpY = m_canvas.vpY;
    m_camera3d.vpW = m_canvas.vpW;
    m_camera3d.vpH = m_canvas.vpH;

    // Update player movement
    float dt = ImGui::GetIO().DeltaTime;
    m_playerMarker.Update(dt);

    // Camera follow mode: track player position
    if (m_playerMarker.placed)
        m_camera3d.UpdateFollow(m_playerMarker.posX, m_playerMarker.posY, m_playerMarker.posZ);

    // Process 3D camera input
    m_camera3d.ProcessInput();
    m_camera3d.ComputeMatrices();

    // Ensure navmesh tiles around player for pathfinding (even when layer hidden)
    if (m_playerMarker.placed && m_tileCache.GetNavMesh())
        m_tileCache.EnsurePathfindingTiles(m_playerMarker.posX, m_playerMarker.posY);

    // Update tile cache for 3D viewport (only when navmesh layer visible)
    if (m_layers.showNavmesh && m_tileCache.GetNavMesh()) {
        m_tileCache.SetMaxTiles(m_layers.navmeshMaxTiles);
        m_tileCache.UpdateViewport3D(m_camera3d.targetX, m_camera3d.targetY,
                                      m_camera3d.distance);
    }

    // Clear backbuffer + depth BEFORE 3D scene rendering
    if (m_rtv) {
        const float clear_color[4] = { 0.1f, 0.1f, 0.12f, 1.0f };
        m_context->OMSetRenderTargets(1, &m_rtv, m_dsv);
        m_context->ClearRenderTargetView(m_rtv, clear_color);
        if (m_dsv)
            m_context->ClearDepthStencilView(m_dsv, D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.0f, 0);
    }

    // Render ground plane (behind navmesh — depth write disabled)
    if (m_layers.showGroundPlane && m_wowDirSet && m_rtv && m_dsv)
        m_groundPlane3d.Render(m_camera3d, m_minimapCache);

    // Render terrain heightmap (opaque, depth write ON — BEFORE navmesh)
    if (m_layers.showTerrain && m_mmapDirSet && m_rtv && m_dsv) {
        m_terrainRenderer.colorMode = m_layers.terrainColorMode;
        m_terrainRenderer.UpdateViewport(m_currentMapId,
                                          m_camera3d.targetX, m_camera3d.targetY,
                                          m_camera3d.distance);
        m_terrainRenderer.Render(m_camera3d, m_rtv, m_dsv);
    }

    // Render buildings (opaque, depth write ON — BEFORE navmesh, AFTER terrain)
    if (m_layers.showBuildings && m_mmapDirSet && m_rtv && m_dsv) {
        m_buildingRenderer.SetShowObjects(m_layers.showBuildingObjects);
        m_buildingRenderer.SetPortalCulling(m_layers.enablePortalCulling);
        if (m_wowDirSet)
            m_buildingRenderer.SetMpqArchive(&m_mpq);
        m_buildingRenderer.UpdateViewport(m_currentMapId,
                                           m_camera3d.targetX, m_camera3d.targetY,
                                           m_camera3d.distance);
        m_buildingRenderer.Render(m_camera3d, m_rtv, m_dsv);
    }

    // Render 3D navmesh (direct DX11 draws before ImGui)
    if (m_layers.showNavmesh && m_rtv && m_dsv) {
        auto colorMode = static_cast<NavmeshColorMode>(m_layers.navmeshColorMode);
        m_navmeshRenderer3d.Render(m_camera3d, m_tileCache, m_rtv, m_dsv,
                                    colorMode, m_layers.navmeshDrawEdges);
    }

    // Phase 3: render 3D overlays via Primitives3D
    m_primitives3d.BeginFrame();

    if (m_layers.showGrid)
        m_gridRenderer3d.Render(m_camera3d, m_primitives3d, m_tileIndex, m_currentMapId);

    if (m_graphData.IsLoaded())
        m_graphRenderer3d.Render(m_camera3d, m_primitives3d, m_graphData, m_currentMapId, m_selection, m_layers);

    if (m_layers.showRoutes)
        m_routeRenderer3d.Render(m_camera3d, m_primitives3d, m_routeData, m_routeEditor, m_currentMapId);

    if (m_layers.showPath)
        m_pathRenderer3d.Render(m_camera3d, m_primitives3d, m_pathRenderer);

    // Render player marker
    if (m_playerMarker.placed) {
        float px = m_playerMarker.posX;
        float py = m_playerMarker.posY;
        float pz = m_playerMarker.posZ;

        // Sphere at player position (cyan)
        m_primitives3d.AddCircle(px, py, pz + 0.5f, 3.0f, IM_COL32(0, 220, 255, 220), 20);
        m_primitives3d.AddCircle(px, py, pz + 2.0f, 2.0f, IM_COL32(0, 220, 255, 180), 16);

        // Vertical line (height indicator)
        m_primitives3d.AddLine(px, py, pz, px, py, pz + 4.0f, IM_COL32(0, 220, 255, 150));

        // Facing direction line
        float facingLen = 6.0f;
        float fx = px + facingLen * cosf(m_playerMarker.facing);
        float fy = py + facingLen * sinf(m_playerMarker.facing);
        m_primitives3d.AddLine(px, py, pz + 1.0f, fx, fy, pz + 1.0f, IM_COL32(255, 255, 0, 200));

        // Movement path preview
        if (m_playerMarker.isMoving && !m_playerMarker.route.empty()) {
            // Draw from current pos to current waypoint, then rest of route
            float prevX = px, prevY = py, prevZ = pz;
            for (int i = m_playerMarker.currentWaypointIndex;
                 i < static_cast<int>(m_playerMarker.route.size()); ++i) {
                const auto& wp = m_playerMarker.route[i];
                m_primitives3d.AddLine(prevX, prevY, prevZ + 0.3f,
                                       wp.x, wp.y, wp.z + 0.3f,
                                       IM_COL32(0, 220, 255, 100));
                prevX = wp.x; prevY = wp.y; prevZ = wp.z;
            }
        }

        // Label via ImGui
        float sx, sy;
        if (m_camera3d.WorldToScreen(px, py, pz + 5.0f, sx, sy)) {
            auto* fgDL = ImGui::GetForegroundDrawList();
            char buf[64];
            snprintf(buf, sizeof(buf), "Player (%.0f, %.0f, %.0f)", px, py, pz);
            fgDL->AddText(ImVec2(sx - 40.0f, sy - 10.0f),
                          IM_COL32(0, 220, 255, 220), buf);
        }
    }

    m_primitives3d.Flush(&m_camera3d.viewProj._11);

    // === 3D mouse interaction: Ctrl+Click teleport, Shift+Click pathfind ===
    {
        auto& io = ImGui::GetIO();
        bool mouseInViewport = (io.MousePos.x >= m_canvas.vpX &&
                                io.MousePos.x <= m_canvas.vpX + m_canvas.vpW &&
                                io.MousePos.y >= m_canvas.vpY &&
                                io.MousePos.y <= m_canvas.vpY + m_canvas.vpH);

        if (mouseInViewport && !io.WantCaptureMouse &&
            ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            bool ctrlDown  = io.KeyCtrl;
            bool shiftDown = io.KeyShift;

            if (ctrlDown || shiftDown) {
                // Cast ray from mouse into the scene
                float ox, oy, oz, dx, dy, dz;
                m_camera3d.ScreenToRay(io.MousePos.x, io.MousePos.y,
                                        ox, oy, oz, dx, dy, dz);

                // Simple ground-plane intersection: find t where oz + dz*t = targetZ (approx ground)
                // Better: find t where ray hits Z ≈ camera target Z or Z=0
                float groundZ = m_camera3d.targetZ;
                if (fabsf(dz) > 0.001f) {
                    float t = (groundZ - oz) / dz;
                    if (t > 0.0f) {
                        float hitX = ox + dx * t;
                        float hitY = oy + dy * t;
                        float hitZ = groundZ;

                        if (ctrlDown) {
                            // Ctrl+Click: teleport player
                            m_playerMarker.SetPosition(hitX, hitY, hitZ);
                        } else if (shiftDown && m_playerMarker.placed) {
                            // Shift+Click: pathfind to clicked point
                            auto* query = m_tileCache.GetQuery();
                            if (query)
                                m_playerMarker.SetDestination(hitX, hitY, hitZ,
                                                              m_pathfinder, query);
                        }
                    }
                }
            }
        }
    }

    // === UI Panels (same as 2D) ===
    UndoContext undo{m_undoRedo, m_graphData, m_routeData};
    m_propertyPanel.Render(m_graphData, m_selection, m_currentMapId,
                           m_canvas, m_tileCache, undo);
    m_routeEditor.RenderPanel(m_routeData, m_currentMapId, undo);
    m_layerPanel.Render(m_layers, m_bgMode, &m_minimapCache);
    m_logWindow.Render();

    // === Player Control Panel (3D only) ===
    {
        ImGuiViewport* pvp = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(
            ImVec2(pvp->WorkPos.x + pvp->WorkSize.x - 280,
                   pvp->WorkPos.y + 36.0f + 250.0f), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(260, 0), ImGuiCond_FirstUseEver);
        if (ImGui::Begin("Player", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            if (m_playerMarker.placed) {
                ImGui::Text("Pos: %.1f, %.1f, %.1f",
                            m_playerMarker.posX, m_playerMarker.posY, m_playerMarker.posZ);
                ImGui::Text("Facing: %.1f deg",
                            m_playerMarker.facing * 180.0f / 3.14159265f);

                ImGui::Separator();

                // Speed control
                ImGui::Text("Speed:");
                ImGui::SameLine();
                float spd = m_playerMarker.speedMultiplier;
                if (ImGui::RadioButton("1x", spd == 1.0f)) m_playerMarker.speedMultiplier = 1.0f;
                ImGui::SameLine();
                if (ImGui::RadioButton("2x", spd == 2.0f)) m_playerMarker.speedMultiplier = 2.0f;
                ImGui::SameLine();
                if (ImGui::RadioButton("5x", spd == 5.0f)) m_playerMarker.speedMultiplier = 5.0f;
                ImGui::SameLine();
                if (ImGui::RadioButton("10x", spd == 10.0f)) m_playerMarker.speedMultiplier = 10.0f;

                // Movement state
                if (m_playerMarker.isMoving) {
                    ImGui::TextColored(ImVec4(0, 0.9f, 1, 1), "Moving...");
                    ImGui::SameLine();
                    if (ImGui::Button("Stop"))
                        m_playerMarker.Stop();
                } else {
                    ImGui::TextDisabled("Idle");
                }

                ImGui::Separator();

                // Camera follow toggle
                ImGui::Checkbox("Camera Follow", &m_camera3d.followMode);

                // Route follow
                if (!m_routeData.GetRoutes().empty()) {
                    ImGui::Separator();
                    ImGui::Text("Follow Route:");
                    for (int ri = 0; ri < static_cast<int>(m_routeData.GetRoutes().size()); ++ri) {
                        const auto& route = m_routeData.GetRoutes()[ri];
                        if (route.mapId != m_currentMapId) continue;
                        if (route.waypoints.empty()) continue;
                        char label[128];
                        snprintf(label, sizeof(label), "%s##route%d",
                                 route.name.empty() ? "(unnamed)" : route.name.c_str(), ri);
                        if (ImGui::Button(label)) {
                            std::vector<PlayerMarker::RoutePoint> pts;
                            pts.reserve(route.waypoints.size());
                            for (const auto& wp : route.waypoints)
                                pts.push_back({wp.x, wp.y, wp.z});
                            m_playerMarker.FollowRoute(pts, route.loop);
                        }
                    }
                }

                ImGui::Separator();
                if (ImGui::Button("Remove Player"))
                    m_playerMarker.Clear();
            } else {
                ImGui::TextDisabled("Ctrl+Click on navmesh to place player");
                ImGui::TextDisabled("Shift+Click to pathfind (after placement)");
            }
        }
        ImGui::End();
    }

    // === Compass (3D: rotates with camera yaw, negated for X-flip) ===
    m_compass.Render(m_canvas.vpX, m_canvas.vpY, m_canvas.vpW, m_canvas.vpH, -m_camera3d.yaw);

    // === Status bar (3D mode) ===
    const char* mapName = "No Map";
    if (auto* mi = FindMap(m_currentMapId))
        mapName = mi->name;
    m_statusBar.Render3D(m_camera3d, m_currentMapId, mapName);
}

void App::RenderMapSelector() {
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x, vp->WorkPos.y));
    ImGui::SetNextWindowSize(ImVec2(vp->WorkSize.x, 36.0f));
    ImGui::Begin("##Toolbar", nullptr,
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus);

    if (m_mmapDirSet) {
        ImGui::Text("Map:");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(300);

        const char* currentMapName = "Select Map";
        if (auto* mi = FindMap(m_currentMapId))
            currentMapName = mi->name;

        char label[256];
        snprintf(label, sizeof(label), "[%u] %s", m_currentMapId, currentMapName);

        if (ImGui::BeginCombo("##MapCombo", label)) {
            auto mapIds = m_tileIndex.GetMapIds();
            for (uint32_t id : mapIds) {
                const char* name = "Unknown";
                if (auto* mi = FindMap(id))
                    name = mi->name;

                char entry[256];
                snprintf(entry, sizeof(entry), "[%u] %s", id, name);

                bool selected = (id == m_currentMapId);
                if (ImGui::Selectable(entry, selected))
                    m_currentMapId = id;
                if (selected)
                    ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }

        ImGui::SameLine();
        auto& tiles = m_tileIndex.GetTilesForMap(m_currentMapId);
        ImGui::Text("(%d tiles)", static_cast<int>(tiles.size()));

        // Edge mode indicator
        if (m_graphEditor.IsEdgeMode()) {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(1, 0.8f, 0, 1), "[Edge Mode - E to toggle]");
        }

        // Path test state
        if (m_pathRenderer.GetState() == PathTestState::SetA) {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(1, 0.3f, 0.3f, 1), "[Click to set point A]");
        } else if (m_pathRenderer.GetState() == PathTestState::SetB) {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.3f, 1, 0.3f, 1), "[Click to set point B]");
        }

        // Tile cache info
        if (m_tileCache.GetLoadedCount() > 0) {
            ImGui::SameLine();
            ImGui::TextDisabled("Navmesh: %d tiles loaded", m_tileCache.GetLoadedCount());
        }

        // Undo/Redo buttons (right side of toolbar)
        {
            float undoX = vp->WorkPos.x + vp->WorkSize.x - 480;
            ImGui::SameLine(undoX - ImGui::GetCursorPosX() + ImGui::GetCursorPosX());
            ImGui::SetCursorPosX(undoX);

            ImGui::BeginDisabled(!m_undoRedo.CanUndo());
            if (ImGui::Button("Undo"))
                m_undoRedo.Undo(m_graphData, m_routeData);
            ImGui::EndDisabled();

            ImGui::SameLine();
            ImGui::BeginDisabled(!m_undoRedo.CanRedo());
            if (ImGui::Button("Redo"))
                m_undoRedo.Redo(m_graphData, m_routeData);
            ImGui::EndDisabled();

            ImGui::SameLine();
            ImGui::TextDisabled("%zu/%zu", m_undoRedo.UndoCount(), UndoRedo::kMaxDepth);
        }

        // Performance overlay (right of undo/redo)
        {
            static float s_cpuPercent = 0.0f;
            static int   s_ramMB = 0;
            static int   s_vramMB = 0;
            static ULONGLONG s_lastPerfTick = 0;
            static ULARGE_INTEGER s_lastKernel = {}, s_lastUser = {};

            ULONGLONG now = GetTickCount64();
            if (now - s_lastPerfTick >= 500) {
                // CPU usage
                FILETIME ftCreate, ftExit, ftKernel, ftUser;
                if (GetProcessTimes(GetCurrentProcess(), &ftCreate, &ftExit, &ftKernel, &ftUser)) {
                    ULARGE_INTEGER kernel, user;
                    kernel.LowPart = ftKernel.dwLowDateTime;
                    kernel.HighPart = ftKernel.dwHighDateTime;
                    user.LowPart = ftUser.dwLowDateTime;
                    user.HighPart = ftUser.dwHighDateTime;

                    if (s_lastPerfTick > 0) {
                        ULONGLONG cpuTime100ns = (kernel.QuadPart - s_lastKernel.QuadPart)
                                               + (user.QuadPart - s_lastUser.QuadPart);
                        ULONGLONG elapsedMs = now - s_lastPerfTick;
                        SYSTEM_INFO si;
                        GetSystemInfo(&si);
                        s_cpuPercent = static_cast<float>(cpuTime100ns / 10000.0)
                                     / static_cast<float>(elapsedMs * si.dwNumberOfProcessors) * 100.0f;
                    }
                    s_lastKernel = kernel;
                    s_lastUser = user;
                }

                // RAM (working set)
                PROCESS_MEMORY_COUNTERS pmc = {};
                pmc.cb = sizeof(pmc);
                if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc)))
                    s_ramMB = static_cast<int>(pmc.WorkingSetSize / (1024 * 1024));

                // VRAM (DXGI dedicated GPU memory)
                if (m_device) {
                    IDXGIDevice* dxgiDev = nullptr;
                    if (SUCCEEDED(m_device->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(&dxgiDev)))) {
                        IDXGIAdapter* adapter = nullptr;
                        if (SUCCEEDED(dxgiDev->GetAdapter(&adapter))) {
                            IDXGIAdapter3* adapter3 = nullptr;
                            if (SUCCEEDED(adapter->QueryInterface(__uuidof(IDXGIAdapter3), reinterpret_cast<void**>(&adapter3)))) {
                                DXGI_QUERY_VIDEO_MEMORY_INFO memInfo = {};
                                if (SUCCEEDED(adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &memInfo)))
                                    s_vramMB = static_cast<int>(memInfo.CurrentUsage / (1024 * 1024));
                                adapter3->Release();
                            }
                            adapter->Release();
                        }
                        dxgiDev->Release();
                    }
                }

                s_lastPerfTick = now;
            }

            float fps = ImGui::GetIO().Framerate;

            ImGui::SameLine(vp->WorkSize.x - 280);
            // Color FPS based on value
            if (fps >= 30.0f)
                ImGui::TextColored(ImVec4(0.5f, 0.8f, 0.5f, 1.0f), "%.0f fps", fps);
            else if (fps >= 15.0f)
                ImGui::TextColored(ImVec4(0.9f, 0.9f, 0.3f, 1.0f), "%.0f fps", fps);
            else
                ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "%.0f fps", fps);

            ImGui::SameLine();
            ImGui::TextDisabled("CPU %.0f%%", s_cpuPercent);
            ImGui::SameLine();
            ImGui::TextDisabled("RAM %dM", s_ramMB);
            ImGui::SameLine();
            ImGui::TextDisabled("GPU %dM", s_vramMB);
        }
    } else {
        ImGui::TextDisabled("Open File > Open MMAP Directory to begin");
    }

    ImGui::End();
}

void App::Shutdown() {
    SaveSettings();

    m_navmeshRenderer.Shutdown();
    m_navmeshRenderer3d.Shutdown();
    m_primitives3d.Shutdown();
    m_groundPlane3d.Shutdown();
    m_terrainRenderer.Shutdown();
    m_buildingRenderer.Shutdown();
    m_minimapCache.Shutdown();
    m_tileCache.Clear();
    m_mapBackground.Release();
    m_mpq.Close();

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    CleanupDX11();
    if (m_hwnd) DestroyWindow(m_hwnd);
    UnregisterClassW(L"MapEditorClass", GetModuleHandle(nullptr));

    LOG(INFO) << "[App] Shutdown complete";
    m_logWindow.Shutdown();
    google::ShutdownGoogleLogging();
    CoUninitialize();
}

void App::LoadSettings() {
    m_settings.Load();
}

void App::SaveSettings() {
    m_settings.mmapDir = m_mmapDir;
    // Get WoW directory from MPQ archive set (stored internally)
    // We don't store it separately, so we save what we have in settings
    if (m_graphData.IsLoaded())
        m_settings.worldGraphPath = m_graphData.GetFilePath();
    if (m_routeData.IsLoaded())
        m_settings.routesPath = m_routeData.GetFilePath();
    m_settings.lastMapId = m_currentMapId;
    m_settings.bgOpacity = m_mapBackground.GetOpacity();
    m_settings.autoBgFromMpq = m_autoBgFromMpq;
    m_settings.showLegend = m_showLegend;
    m_settings.bgMode = static_cast<int>(m_bgMode);

    // Layer visibility
    m_settings.showGrid = m_layers.showGrid;
    m_settings.showNavmesh = m_layers.showNavmesh;
    m_settings.showNodes = m_layers.showNodes;
    m_settings.showEdges = m_layers.showEdges;
    m_settings.showLabels = m_layers.showLabels;
    m_settings.showRoutes = m_layers.showRoutes;
    m_settings.showPath = m_layers.showPath;
    m_settings.showBackground = m_layers.showBackground;
    m_settings.navmeshMinZoom = m_layers.navmeshMinZoom;
    m_settings.navmeshMaxTiles = m_layers.navmeshMaxTiles;
    m_settings.showTerrain = m_layers.showTerrain;
    m_settings.terrainColorMode = m_layers.terrainColorMode;
    m_settings.showBuildings = m_layers.showBuildings;
    m_settings.showBuildingObjects = m_layers.showBuildingObjects;
    m_settings.enablePortalCulling = m_layers.enablePortalCulling;

    // Window dimensions
    if (m_hwnd) {
        RECT r;
        if (GetWindowRect(m_hwnd, &r)) {
            m_settings.windowWidth = r.right - r.left;
            m_settings.windowHeight = r.bottom - r.top;
        }
    }

    m_settings.Save();
}

LRESULT CALLBACK App::WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam))
        return true;

    if (s_app) {
        switch (msg) {
        case WM_SIZE:
            if (wParam != SIZE_MINIMIZED) {
                s_app->m_resizeWidth = LOWORD(lParam);
                s_app->m_resizeHeight = HIWORD(lParam);
            }
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        }
    }

    return DefWindowProc(hWnd, msg, wParam, lParam);
}

} // namespace mapedit
