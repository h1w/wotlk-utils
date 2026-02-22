#include "map_background.h"
#include "../canvas/canvas.h"

#include <windows.h>
#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif

#include <d3d11.h>
#include <gdiplus.h>
#include <imgui.h>
#include <glog/logging.h>

#pragma comment(lib, "gdiplus.lib")

namespace mapedit {

bool MapBackground::CreateTexture(ID3D11Device* device, const void* pixels,
                                   int width, int height, int pitch, bool isBgra) {
    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = isBgra ? DXGI_FORMAT_B8G8R8A8_UNORM : DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    D3D11_SUBRESOURCE_DATA initData = {};
    initData.pSysMem = pixels;
    initData.SysMemPitch = pitch;

    ID3D11Texture2D* tex = nullptr;
    HRESULT hr = device->CreateTexture2D(&desc, &initData, &tex);
    if (FAILED(hr)) {
        LOG(ERROR) << "[MapBackground] CreateTexture2D failed: 0x" << std::hex << hr;
        return false;
    }

    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = desc.Format;
    srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MipLevels = 1;

    Release();
    hr = device->CreateShaderResourceView(tex, &srvDesc, &m_srv);
    tex->Release();

    if (FAILED(hr)) {
        LOG(ERROR) << "[MapBackground] CreateSRV failed: 0x" << std::hex << hr;
        return false;
    }

    m_width = width;
    m_height = height;
    return true;
}

bool MapBackground::LoadFromFile(ID3D11Device* device, const std::string& path,
                                  float worldMinX, float worldMaxX,
                                  float worldMinY, float worldMaxY) {
    Gdiplus::GdiplusStartupInput si;
    ULONG_PTR token = 0;
    if (Gdiplus::GdiplusStartup(&token, &si, nullptr) != Gdiplus::Ok) {
        LOG(ERROR) << "[MapBackground] GDI+ init failed";
        return false;
    }

    wchar_t wpath[MAX_PATH];
    MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, wpath, MAX_PATH);

    bool result = false;
    {
        Gdiplus::Bitmap bmp(wpath);
        if (bmp.GetLastStatus() != Gdiplus::Ok) {
            LOG(ERROR) << "[MapBackground] Failed to load image: " << path;
            Gdiplus::GdiplusShutdown(token);
            return false;
        }

        int w = static_cast<int>(bmp.GetWidth());
        int h = static_cast<int>(bmp.GetHeight());
        if (w == 0 || h == 0) {
            Gdiplus::GdiplusShutdown(token);
            return false;
        }

        Gdiplus::BitmapData bd = {};
        Gdiplus::Rect rect(0, 0, w, h);
        if (bmp.LockBits(&rect, Gdiplus::ImageLockModeRead,
                          PixelFormat32bppARGB, &bd) != Gdiplus::Ok) {
            LOG(ERROR) << "[MapBackground] LockBits failed";
            Gdiplus::GdiplusShutdown(token);
            return false;
        }

        // GDI+ ARGB = BGRA in memory
        result = CreateTexture(device, bd.Scan0, w, h, bd.Stride, true);
        bmp.UnlockBits(&bd);

        if (result) {
            m_worldMinX = worldMinX;
            m_worldMaxX = worldMaxX;
            m_worldMinY = worldMinY;
            m_worldMaxY = worldMaxY;
            LOG(INFO) << "[MapBackground] Loaded " << w << "x" << h
                      << " from file: " << path;
        }
    }

    Gdiplus::GdiplusShutdown(token);
    return result;
}

bool MapBackground::LoadFromPixels(ID3D11Device* device, const uint8_t* bgra,
                                    int width, int height,
                                    float worldMinX, float worldMaxX,
                                    float worldMinY, float worldMaxY) {
    if (!bgra || width <= 0 || height <= 0) return false;

    if (!CreateTexture(device, bgra, width, height, width * 4, true))
        return false;

    m_worldMinX = worldMinX;
    m_worldMaxX = worldMaxX;
    m_worldMinY = worldMinY;
    m_worldMaxY = worldMaxY;
    return true;
}

void MapBackground::Render(const Canvas& canvas) {
    if (!m_srv) return;

    auto* dl = ImGui::GetBackgroundDrawList();

    // Northwest corner = (maxX, maxY), southeast = (minX, minY)
    float sx1, sy1, sx2, sy2;
    canvas.WorldToScreen(m_worldMaxX, m_worldMaxY, sx1, sy1);
    canvas.WorldToScreen(m_worldMinX, m_worldMinY, sx2, sy2);

    ImU32 tint = IM_COL32(255, 255, 255, static_cast<int>(m_opacity * 255));
    dl->AddImage(reinterpret_cast<ImTextureID>(m_srv),
                 ImVec2(sx1, sy1), ImVec2(sx2, sy2),
                 ImVec2(0, 0), ImVec2(1, 1), tint);
}

void MapBackground::Release() {
    if (m_srv) {
        m_srv->Release();
        m_srv = nullptr;
    }
    m_width = m_height = 0;
}

} // namespace mapedit
