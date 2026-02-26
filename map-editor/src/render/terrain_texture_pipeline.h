#pragma once

#include <d3d11.h>

namespace mapedit {

// Textured terrain pipeline. Uses the same TerrainCB constant buffer layout
// as TerrainPipeline. When heightParams.z (colorMode) == 3, the PS samples
// the bound texture atlas; otherwise falls back to procedural color.
class TerrainTexturePipeline {
public:
    bool Initialize(ID3D11Device* device);
    void Shutdown();

    ID3D11VertexShader*      GetVS()        const { return m_vs; }
    ID3D11PixelShader*       GetPS()        const { return m_ps; }
    ID3D11InputLayout*       GetLayout()    const { return m_layout; }
    ID3D11RasterizerState*   GetRastState() const { return m_rastState; }
    ID3D11DepthStencilState* GetDSState()   const { return m_dsState; }
    ID3D11BlendState*        GetBlendState() const { return m_blendState; }
    ID3D11Buffer*            GetCB()        const { return m_cb; }
    ID3D11SamplerState*      GetSampler()   const { return m_sampler; }
    bool IsReady() const { return m_vs != nullptr; }

private:
    ID3D11VertexShader*      m_vs = nullptr;
    ID3D11PixelShader*       m_ps = nullptr;
    ID3D11InputLayout*       m_layout = nullptr;
    ID3D11RasterizerState*   m_rastState = nullptr;
    ID3D11DepthStencilState* m_dsState = nullptr;
    ID3D11BlendState*        m_blendState = nullptr;
    ID3D11Buffer*            m_cb = nullptr;
    ID3D11SamplerState*      m_sampler = nullptr;
};

} // namespace mapedit
