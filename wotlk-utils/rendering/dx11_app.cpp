#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#undef FormatMessage

#include "dx11_app.hpp"

#include <d3d11.h>
#include <dxgi.h>

#include <imgui.h>
#include <imgui_impl_dx11.h>
#include <imgui_impl_win32.h>

#include <glog/logging.h>

// Forward declare ImGui Win32 handler
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace
{

// Module state
HWND                     g_hwnd = nullptr;
WNDCLASSEXW              g_wc = {};
ID3D11Device*            g_pd3dDevice = nullptr;
ID3D11DeviceContext*     g_pd3dDeviceContext = nullptr;
IDXGISwapChain*          g_pSwapChain = nullptr;
ID3D11RenderTargetView*  g_pRenderTargetView = nullptr;
bool                     g_initialized = false;

void CreateRenderTarget()
{
	ID3D11Texture2D* pBackBuffer = nullptr;
	g_pSwapChain->GetBuffer(0, IID_PPV_ARGS(&pBackBuffer));
	if (pBackBuffer)
	{
		g_pd3dDevice->CreateRenderTargetView(pBackBuffer, nullptr, &g_pRenderTargetView);
		pBackBuffer->Release();
	}
}

void CleanupRenderTarget()
{
	if (g_pRenderTargetView)
	{
		g_pRenderTargetView->Release();
		g_pRenderTargetView = nullptr;
	}
}

bool CreateDeviceD3D(HWND hWnd)
{
	DXGI_SWAP_CHAIN_DESC sd = {};
	sd.BufferCount = 2;
	sd.BufferDesc.Width = 0;
	sd.BufferDesc.Height = 0;
	sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	sd.BufferDesc.RefreshRate.Numerator = 60;
	sd.BufferDesc.RefreshRate.Denominator = 1;
	sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
	sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
	sd.OutputWindow = hWnd;
	sd.SampleDesc.Count = 1;
	sd.SampleDesc.Quality = 0;
	sd.Windowed = TRUE;
	sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

	UINT createDeviceFlags = 0;
#ifdef _DEBUG
	createDeviceFlags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

	D3D_FEATURE_LEVEL featureLevel;
	const D3D_FEATURE_LEVEL featureLevelArray[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0 };
	HRESULT hr = D3D11CreateDeviceAndSwapChain(
		nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
		createDeviceFlags, featureLevelArray, 2,
		D3D11_SDK_VERSION, &sd,
		&g_pSwapChain, &g_pd3dDevice, &featureLevel, &g_pd3dDeviceContext);
	if (hr == DXGI_ERROR_UNSUPPORTED)
	{
		hr = D3D11CreateDeviceAndSwapChain(
			nullptr, D3D_DRIVER_TYPE_WARP, nullptr,
			createDeviceFlags, featureLevelArray, 2,
			D3D11_SDK_VERSION, &sd,
			&g_pSwapChain, &g_pd3dDevice, &featureLevel, &g_pd3dDeviceContext);
	}
	if (FAILED(hr))
		return false;

	CreateRenderTarget();
	return true;
}

void CleanupDeviceD3D()
{
	CleanupRenderTarget();
	if (g_pSwapChain)     { g_pSwapChain->Release();         g_pSwapChain = nullptr; }
	if (g_pd3dDeviceContext) { g_pd3dDeviceContext->Release(); g_pd3dDeviceContext = nullptr; }
	if (g_pd3dDevice)     { g_pd3dDevice->Release();         g_pd3dDevice = nullptr; }
}

LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
	if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam))
		return true;

	switch (msg)
	{
	case WM_SIZE:
		if (wParam == SIZE_MINIMIZED)
			return 0;
		if (g_pd3dDevice != nullptr)
		{
			CleanupRenderTarget();
			g_pSwapChain->ResizeBuffers(0,
				static_cast<UINT>(LOWORD(lParam)),
				static_cast<UINT>(HIWORD(lParam)),
				DXGI_FORMAT_UNKNOWN, 0);
			CreateRenderTarget();
		}
		return 0;
	case WM_DESTROY:
		PostQuitMessage(0);
		return 0;
	default:
		break;
	}
	return DefWindowProcW(hWnd, msg, wParam, lParam);
}

} // anonymous namespace

namespace dx11_app
{

void Initialize(const AppConfig& config)
{
	LOG(INFO) << "Initializing DX11 application";

	// Register Win32 window class
	g_wc = {};
	g_wc.cbSize = sizeof(WNDCLASSEXW);
	g_wc.style = CS_CLASSDC;
	g_wc.lpfnWndProc = WndProc;
	g_wc.hInstance = GetModuleHandleW(nullptr);
	g_wc.lpszClassName = L"WotlkUtilsDX11";
	RegisterClassExW(&g_wc);

	// Convert title to wide string
	int titleLen = MultiByteToWideChar(CP_UTF8, 0, config.window_title.c_str(), -1, nullptr, 0);
	std::wstring wideTitle(titleLen, L'\0');
	MultiByteToWideChar(CP_UTF8, 0, config.window_title.c_str(), -1, wideTitle.data(), titleLen);

	// Create window
	g_hwnd = CreateWindowExW(
		0, g_wc.lpszClassName, wideTitle.c_str(),
		WS_OVERLAPPEDWINDOW,
		CW_USEDEFAULT, CW_USEDEFAULT,
		config.window_width, config.window_height,
		nullptr, nullptr, g_wc.hInstance, nullptr);

	if (!g_hwnd)
	{
		LOG(ERROR) << "Failed to create Win32 window";
		return;
	}

	// Create D3D11 device and swap chain
	if (!CreateDeviceD3D(g_hwnd))
	{
		LOG(ERROR) << "Failed to create D3D11 device";
		CleanupDeviceD3D();
		DestroyWindow(g_hwnd);
		UnregisterClassW(g_wc.lpszClassName, g_wc.hInstance);
		return;
	}

	// Show window
	ShowWindow(g_hwnd, SW_SHOWDEFAULT);
	UpdateWindow(g_hwnd);

	// Setup ImGui context
	IMGUI_CHECKVERSION();
	ImGui::CreateContext();
	ImGuiIO& io = ImGui::GetIO();
	io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

	// Setup ImGui style
	ImGui::StyleColorsDark();

	// Setup backends
	ImGui_ImplWin32_Init(g_hwnd);
	ImGui_ImplDX11_Init(g_pd3dDevice, g_pd3dDeviceContext);

	g_initialized = true;
	LOG(INFO) << "DX11 application initialized (" << config.window_width << "x" << config.window_height << ")";
}

void Run(void (*frameFn)())
{
	if (!g_initialized)
	{
		LOG(ERROR) << "dx11_app::Run called before successful initialization";
		return;
	}

	LOG(INFO) << "Entering main loop";

	const ImVec4 clearColor(0.45f, 0.55f, 0.60f, 1.00f);
	bool running = true;

	while (running)
	{
		MSG msg;
		while (PeekMessageW(&msg, nullptr, 0U, 0U, PM_REMOVE))
		{
			TranslateMessage(&msg);
			DispatchMessageW(&msg);
			if (msg.message == WM_QUIT)
				running = false;
		}
		if (!running)
			break;

		// Skip rendering if minimized
		if (IsIconic(g_hwnd))
		{
			Sleep(10);
			continue;
		}

		// Start ImGui frame
		ImGui_ImplDX11_NewFrame();
		ImGui_ImplWin32_NewFrame();
		ImGui::NewFrame();

		// User content or demo window
		if (frameFn)
			frameFn();
		else
			ImGui::ShowDemoWindow();

		// Render
		ImGui::Render();
		const float clearColorArray[4] = {
			clearColor.x * clearColor.w,
			clearColor.y * clearColor.w,
			clearColor.z * clearColor.w,
			clearColor.w
		};
		g_pd3dDeviceContext->OMSetRenderTargets(1, &g_pRenderTargetView, nullptr);
		g_pd3dDeviceContext->ClearRenderTargetView(g_pRenderTargetView, clearColorArray);
		ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

		// Present with vsync
		g_pSwapChain->Present(1, 0);
	}

	LOG(INFO) << "Main loop exited";
}

void Shutdown()
{
	LOG(INFO) << "Shutting down DX11 application";

	ImGui_ImplDX11_Shutdown();
	ImGui_ImplWin32_Shutdown();
	ImGui::DestroyContext();

	CleanupDeviceD3D();
	if (g_hwnd)
	{
		DestroyWindow(g_hwnd);
		g_hwnd = nullptr;
	}
	UnregisterClassW(g_wc.lpszClassName, g_wc.hInstance);

	g_initialized = false;
	LOG(INFO) << "DX11 application shut down";
}

} // namespace dx11_app
