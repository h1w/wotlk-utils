#pragma once
#include <d3d11.h>

namespace mapedit {

// Color modes for 3D navmesh rendering
enum class NavmeshColorMode : int {
    FlatGreen = 0,    // Current behavior (green fill + brighter edge)
    HeightGradient,   // Blue(low) -> Green(mid) -> White(high)
    SlopeShading,     // Flat=green, steep=red
    TileColored,      // Each tile a different hue
};

// GPU vertex for 3D navmesh: world XYZ + barycentric coords
struct NavmeshVertex3D {
    float posX, posY, posZ;        // WoW world coords (12 bytes)
    float baryX, baryY, baryZ;    // barycentric coords (12 bytes)
};                                  // Total: 24 bytes

// Constant buffer for 3D navmesh (must be 16-byte aligned)
struct NavmeshCB3D {
    float viewProj[16];   // 4x4 View*Projection matrix (64 bytes) — row-major for HLSL
    float fillColor[4];   // RGBA (16 bytes)
    float edgeColor[4];   // RGBA (16 bytes)
    float lightDir[4];    // directional light direction (16 bytes), w=unused
    float colorParams[4]; // [0]=colorMode (0-3), [1]=heightMin, [2]=heightMax, [3]=drawEdges (0 or 1)
    float tileHue[4];     // [0]=tile hue (0-1), [1-3]=unused — for tile-colored mode
};                         // Total: 160 bytes

class NavmeshPipeline3D {
public:
    bool Initialize(ID3D11Device* device);
    void Shutdown();

    ID3D11VertexShader*      GetVS()            const { return m_vs; }
    ID3D11PixelShader*       GetPS()            const { return m_ps; }
    ID3D11InputLayout*       GetInputLayout()   const { return m_inputLayout; }
    ID3D11BlendState*        GetBlendState()    const { return m_blendState; }
    ID3D11RasterizerState*   GetRastState()     const { return m_rastState; }
    ID3D11DepthStencilState* GetDSState()       const { return m_dsState; }
    ID3D11Buffer*            GetConstantBuffer() const { return m_cb; }
    bool IsReady() const { return m_vs != nullptr; }

private:
    ID3D11VertexShader*      m_vs = nullptr;
    ID3D11PixelShader*       m_ps = nullptr;
    ID3D11InputLayout*       m_inputLayout = nullptr;
    ID3D11BlendState*        m_blendState = nullptr;
    ID3D11RasterizerState*   m_rastState = nullptr;
    ID3D11DepthStencilState* m_dsState = nullptr;
    ID3D11Buffer*            m_cb = nullptr;
};

} // namespace mapedit
