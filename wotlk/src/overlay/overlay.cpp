#include "overlay.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d9.h>

#include <imgui.h>
#include <imgui_impl_dx9.h>
#include <imgui_impl_win32.h>

#include <MinHook.h>
#include <glog/logging.h>

// Forward declaration from imgui_impl_win32.cpp
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace overlay {

// EndScene: index 42 in IDirect3DDevice9 vtable
using EndScene_t = HRESULT(WINAPI*)(IDirect3DDevice9*);
// Reset: index 16 in IDirect3DDevice9 vtable
using Reset_t = HRESULT(WINAPI*)(IDirect3DDevice9*, D3DPRESENT_PARAMETERS*);

static EndScene_t g_originalEndScene = nullptr;
static Reset_t g_originalReset = nullptr;
static void* g_endSceneTarget = nullptr;
static void* g_resetTarget = nullptr;
static HHOOK g_msgHook = nullptr;
static HWND g_gameHwnd = nullptr;
static bool g_imguiInitialized = false;
static bool g_shutdownRequested = false;

// ---- Input hook via WH_GETMESSAGE ----

static bool IsInputMessage(UINT msg)
{
    switch (msg) {
    case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_LBUTTONDBLCLK:
    case WM_RBUTTONDOWN: case WM_RBUTTONUP: case WM_RBUTTONDBLCLK:
    case WM_MBUTTONDOWN: case WM_MBUTTONUP: case WM_MBUTTONDBLCLK:
    case WM_MOUSEWHEEL: case WM_MOUSEHWHEEL:
    case WM_MOUSEMOVE:
    case WM_KEYDOWN: case WM_KEYUP:
    case WM_CHAR: case WM_SYSKEYDOWN: case WM_SYSKEYUP:
        return true;
    default:
        return false;
    }
}

static LRESULT CALLBACK GetMsgProc(int code, WPARAM wParam, LPARAM lParam)
{
    if (code >= 0 && !g_shutdownRequested && g_imguiInitialized) {
        MSG* msg = reinterpret_cast<MSG*>(lParam);
        if (msg->hwnd == g_gameHwnd && IsInputMessage(msg->message)) {
            // Forward to ImGui
            ImGui_ImplWin32_WndProcHandler(msg->hwnd, msg->message, msg->wParam, msg->lParam);

            ImGuiIO& io = ImGui::GetIO();
            bool consume = false;

            switch (msg->message) {
            case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_LBUTTONDBLCLK:
            case WM_RBUTTONDOWN: case WM_RBUTTONUP: case WM_RBUTTONDBLCLK:
            case WM_MBUTTONDOWN: case WM_MBUTTONUP: case WM_MBUTTONDBLCLK:
            case WM_MOUSEWHEEL: case WM_MOUSEHWHEEL:
                consume = io.WantCaptureMouse;
                break;
            case WM_KEYDOWN: case WM_KEYUP:
            case WM_CHAR: case WM_SYSKEYDOWN: case WM_SYSKEYUP:
                consume = io.WantCaptureKeyboard;
                break;
            }

            if (consume)
                msg->message = WM_NULL; // Neutralize — game won't see it
        }
    }
    return CallNextHookEx(g_msgHook, code, wParam, lParam);
}

// ---- EndScene hook ----

static HRESULT WINAPI HookedEndScene(IDirect3DDevice9* pDevice)
{
    if (g_shutdownRequested)
        return g_originalEndScene(pDevice);

    // One-time ImGui initialization (deferred to first EndScene to get pDevice)
    if (!g_imguiInitialized) {
        ImGui::CreateContext();
        ImGui_ImplWin32_Init(g_gameHwnd);
        ImGui_ImplDX9_Init(pDevice);

        ImGuiIO& io = ImGui::GetIO();
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

        g_imguiInitialized = true;
        LOG(INFO) << "[OVERLAY] ImGui initialized";
    }

    ImGui_ImplDX9_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();

    // Simple menu window
    ImGui::Begin("wotlk-utils");
    if (ImGui::Button("TEST"))
        LOG(INFO) << "[OVERLAY] TEST button pressed";
    ImGui::End();

    ImGui::EndFrame();
    ImGui::Render();
    ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData());

    return g_originalEndScene(pDevice);
}

// ---- Reset hook ----

static HRESULT WINAPI HookedReset(IDirect3DDevice9* pDevice, D3DPRESENT_PARAMETERS* pParams)
{
    if (g_imguiInitialized && !g_shutdownRequested)
        ImGui_ImplDX9_InvalidateDeviceObjects();

    HRESULT hr = g_originalReset(pDevice, pParams);

    if (g_imguiInitialized && !g_shutdownRequested && SUCCEEDED(hr))
        ImGui_ImplDX9_CreateDeviceObjects();

    return hr;
}

// ---- vtable discovery via dummy device ----

static bool GetD3D9VtableAddresses(void*& outEndScene, void*& outReset)
{
    // Create dummy window
    WNDCLASSEXA wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = DefWindowProcA;
    wc.hInstance = GetModuleHandleA(nullptr);
    wc.lpszClassName = "ImGuiDummyD3D9";
    RegisterClassExA(&wc);

    HWND hDummy = CreateWindowExA(0, wc.lpszClassName, "", WS_OVERLAPPEDWINDOW,
        0, 0, 100, 100, nullptr, nullptr, wc.hInstance, nullptr);
    if (!hDummy) {
        LOG(ERROR) << "[OVERLAY] Failed to create dummy window";
        UnregisterClassA(wc.lpszClassName, wc.hInstance);
        return false;
    }

    IDirect3D9* pD3D = Direct3DCreate9(D3D_SDK_VERSION);
    if (!pD3D) {
        LOG(ERROR) << "[OVERLAY] Direct3DCreate9 failed";
        DestroyWindow(hDummy);
        UnregisterClassA(wc.lpszClassName, wc.hInstance);
        return false;
    }

    D3DPRESENT_PARAMETERS pp = {};
    pp.Windowed = TRUE;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.hDeviceWindow = hDummy;

    IDirect3DDevice9* pDummyDevice = nullptr;
    HRESULT hr = pD3D->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hDummy,
        D3DCREATE_SOFTWARE_VERTEXPROCESSING, &pp, &pDummyDevice);
    if (FAILED(hr)) {
        LOG(ERROR) << "[OVERLAY] CreateDevice failed: 0x" << std::hex << hr;
        pD3D->Release();
        DestroyWindow(hDummy);
        UnregisterClassA(wc.lpszClassName, wc.hInstance);
        return false;
    }

    // Read vtable
    void** vtable = *(void***)pDummyDevice;
    outEndScene = vtable[42];
    outReset = vtable[16];

    LOG(INFO) << "[OVERLAY] EndScene @ 0x" << std::hex << (uintptr_t)outEndScene;
    LOG(INFO) << "[OVERLAY] Reset @ 0x" << std::hex << (uintptr_t)outReset;

    pDummyDevice->Release();
    pD3D->Release();
    DestroyWindow(hDummy);
    UnregisterClassA(wc.lpszClassName, wc.hInstance);
    return true;
}

// ---- Find game window ----

struct FindWindowData {
    DWORD pid;
    HWND result;
};

static BOOL CALLBACK EnumWindowsCallback(HWND hWnd, LPARAM lParam)
{
    auto* data = reinterpret_cast<FindWindowData*>(lParam);
    DWORD windowPid = 0;
    GetWindowThreadProcessId(hWnd, &windowPid);
    if (windowPid == data->pid && IsWindowVisible(hWnd)) {
        data->result = hWnd;
        return FALSE;
    }
    return TRUE;
}

static HWND FindGameWindow()
{
    // Try WoW's known window class first
    HWND hWnd = FindWindowA("GxWindowClass", nullptr);
    if (hWnd) {
        DWORD pid = 0;
        GetWindowThreadProcessId(hWnd, &pid);
        if (pid == GetCurrentProcessId())
            return hWnd;
    }

    // Fallback: enumerate windows for our PID
    FindWindowData data = { GetCurrentProcessId(), nullptr };
    EnumWindows(EnumWindowsCallback, reinterpret_cast<LPARAM>(&data));
    return data.result;
}

// ---- Public API ----

bool Initialize()
{
    // Find game window
    g_gameHwnd = FindGameWindow();
    if (!g_gameHwnd) {
        LOG(ERROR) << "[OVERLAY] Failed to find game window";
        return false;
    }
    LOG(INFO) << "[OVERLAY] Game window: 0x" << std::hex << (uintptr_t)g_gameHwnd;

    // Discover EndScene/Reset addresses via dummy device
    void* endSceneAddr = nullptr;
    void* resetAddr = nullptr;
    if (!GetD3D9VtableAddresses(endSceneAddr, resetAddr))
        return false;

    // Store targets for Shutdown
    g_endSceneTarget = endSceneAddr;
    g_resetTarget = resetAddr;

    // Hook EndScene
    MH_STATUS status = MH_CreateHook(endSceneAddr, &HookedEndScene,
        reinterpret_cast<void**>(&g_originalEndScene));
    if (status != MH_OK) {
        LOG(ERROR) << "[OVERLAY] MH_CreateHook(EndScene) failed: " << MH_StatusToString(status);
        return false;
    }
    status = MH_EnableHook(endSceneAddr);
    if (status != MH_OK) {
        LOG(ERROR) << "[OVERLAY] MH_EnableHook(EndScene) failed: " << MH_StatusToString(status);
        MH_RemoveHook(endSceneAddr);
        return false;
    }

    // Hook Reset
    status = MH_CreateHook(resetAddr, &HookedReset,
        reinterpret_cast<void**>(&g_originalReset));
    if (status != MH_OK) {
        LOG(ERROR) << "[OVERLAY] MH_CreateHook(Reset) failed: " << MH_StatusToString(status);
        MH_DisableHook(endSceneAddr);
        MH_RemoveHook(endSceneAddr);
        return false;
    }
    status = MH_EnableHook(resetAddr);
    if (status != MH_OK) {
        LOG(ERROR) << "[OVERLAY] MH_EnableHook(Reset) failed: " << MH_StatusToString(status);
        MH_RemoveHook(resetAddr);
        MH_DisableHook(endSceneAddr);
        MH_RemoveHook(endSceneAddr);
        return false;
    }

    // Install message hook on the window's thread for input forwarding to ImGui
    DWORD windowThreadId = GetWindowThreadProcessId(g_gameHwnd, nullptr);
    g_msgHook = SetWindowsHookExA(WH_GETMESSAGE, GetMsgProc, nullptr, windowThreadId);
    if (!g_msgHook) {
        LOG(ERROR) << "[OVERLAY] SetWindowsHookEx(WH_GETMESSAGE) failed: " << GetLastError();
        MH_DisableHook(endSceneAddr);
        MH_RemoveHook(endSceneAddr);
        MH_DisableHook(resetAddr);
        MH_RemoveHook(resetAddr);
        return false;
    }
    LOG(INFO) << "[OVERLAY] Message hook installed on thread " << std::dec << windowThreadId;

    LOG(INFO) << "[OVERLAY] Initialized successfully";
    return true;
}

void Shutdown()
{
    g_shutdownRequested = true;
    Sleep(100); // Let in-flight EndScene calls finish

    // Remove message hook
    if (g_msgHook) {
        UnhookWindowsHookEx(g_msgHook);
        g_msgHook = nullptr;
    }

    // Tear down ImGui
    if (g_imguiInitialized) {
        ImGui_ImplDX9_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
        g_imguiInitialized = false;
    }

    // Remove only our MinHook hooks (MH_Uninitialize is called later by hooks::Shutdown)
    if (g_endSceneTarget) {
        MH_DisableHook(g_endSceneTarget);
        MH_RemoveHook(g_endSceneTarget);
        g_endSceneTarget = nullptr;
    }
    if (g_resetTarget) {
        MH_DisableHook(g_resetTarget);
        MH_RemoveHook(g_resetTarget);
        g_resetTarget = nullptr;
    }

    LOG(INFO) << "[OVERLAY] Shut down";
}

} // namespace overlay
