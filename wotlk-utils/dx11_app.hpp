#pragma once

#include <string>

namespace dx11_app
{

struct AppConfig
{
	std::string window_title = "app";
	int window_width = 1280;
	int window_height = 800;
};

// Register Win32 window class, create HWND, create D3D11 device/swap chain,
// init ImGui context + backends.
void Initialize(const AppConfig& config);

// Blocking message loop. Calls frameFn each frame between NewFrame() and Render().
// If frameFn is nullptr, shows the ImGui demo window.
void Run(void (*frameFn)());

// Tear down ImGui, D3D11, Win32 window.
void Shutdown();

} // namespace dx11_app
