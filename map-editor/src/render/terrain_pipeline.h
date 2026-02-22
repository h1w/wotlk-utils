#pragma once

#include <d3d11.h>

namespace mapedit {

// GPU vertex for terrain: world XYZ + normal
struct TerrainVertexGpu {
    float x, y, z;       // Position (12 bytes)
    float nx, ny, nz;    // Normal (12 bytes)
};                        // 24 bytes total

// Constant buffer for terrain rendering (must be 16-byte aligned)
struct TerrainCB {
    float viewProj[16];     // 64 bytes - Transposed VP matrix (column-major HLSL, matching navmesh)
    float lightDir[4];      // 16 bytes - directional light direction (xyz), ambient (w)
    float baseColor[4];     // 16 bytes - terrain fill color RGBA
    float heightParams[4];  // 16 bytes - [0]=minZ, [1]=maxZ, [2]=colorMode, [3]=unused
};                           // 112 bytes

class TerrainPipeline {
public:
    bool Initialize(ID3D11Device* device);
    void Shutdown();

    ID3D11VertexShader*      GetVS()       const { return m_vs; }
    ID3D11PixelShader*       GetPS()       const { return m_ps; }
    ID3D11InputLayout*       GetLayout()   const { return m_layout; }
    ID3D11RasterizerState*   GetRastState() const { return m_rastState; }
    ID3D11DepthStencilState* GetDSState()  const { return m_dsState; }
    ID3D11BlendState*        GetBlendState() const { return m_blendState; }
    ID3D11Buffer*            GetCB()       const { return m_cb; }
    bool IsReady() const { return m_vs != nullptr; }

private:
    ID3D11VertexShader*      m_vs = nullptr;
    ID3D11PixelShader*       m_ps = nullptr;
    ID3D11InputLayout*       m_layout = nullptr;
    ID3D11RasterizerState*   m_rastState = nullptr;
    ID3D11DepthStencilState* m_dsState = nullptr;
    ID3D11BlendState*        m_blendState = nullptr;
    ID3D11Buffer*            m_cb = nullptr;
};

} // namespace mapedit
