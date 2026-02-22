#pragma once

#include <d3d11.h>

namespace mapedit {

// GPU vertex: world XY + barycentric coords for wireframe
struct NavmeshVertex {
    float posX, posY;
    float baryX, baryY, baryZ;
};

// Constant buffer (48 bytes, 16-byte aligned)
struct NavmeshCB {
    float viewCenterX, viewCenterY, scaleX, scaleY; // transform
    float fillR, fillG, fillB, fillA;                // fillColor
    float edgeR, edgeG, edgeB, edgeA;                // edgeColor
};

class NavmeshPipeline {
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
