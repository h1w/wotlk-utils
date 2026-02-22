#pragma once
#include <cstdint>
#include <string>

struct ID3D11Device;
struct ID3D11ShaderResourceView;

namespace mapedit {

struct Canvas;

class MapBackground {
public:
    ~MapBackground() { Release(); }

    // Load from an image file (PNG, BMP, JPG) via GDI+.
    bool LoadFromFile(ID3D11Device* device, const std::string& path,
                      float worldMinX, float worldMaxX,
                      float worldMinY, float worldMaxY);

    // Load from raw BGRA pixel data (e.g., decoded BLP tiles).
    bool LoadFromPixels(ID3D11Device* device, const uint8_t* bgra,
                        int width, int height,
                        float worldMinX, float worldMaxX,
                        float worldMinY, float worldMaxY);

    void Render(const Canvas& canvas);
    void Release();

    bool IsLoaded() const { return m_srv != nullptr; }

    float GetOpacity() const { return m_opacity; }
    void SetOpacity(float a) { m_opacity = a; }

private:
    bool CreateTexture(ID3D11Device* device, const void* pixels, int width, int height,
                       int pitch, bool isBgra);

    ID3D11ShaderResourceView* m_srv = nullptr;
    float m_worldMinX = 0, m_worldMaxX = 0;
    float m_worldMinY = 0, m_worldMaxY = 0;
    float m_opacity = 0.3f;
    int m_width = 0, m_height = 0;
};

} // namespace mapedit
