#pragma once
#include <d3d11.h>

namespace mapedit {

struct Camera3D;
class MinimapTileCache;

class GroundPlane3D {
public:
    bool Initialize(ID3D11Device* device, ID3D11DeviceContext* context);
    void Shutdown();

    // Render minimap textures as ground quads at Z=0
    void Render(const Camera3D& camera, MinimapTileCache& minimapCache);

private:
    struct QuadVertex {
        float x, y, z;  // world position
        float u, v;      // texture coords
    };

    ID3D11Device*            m_device  = nullptr;
    ID3D11DeviceContext*     m_context = nullptr;
    ID3D11VertexShader*      m_vs      = nullptr;
    ID3D11PixelShader*       m_ps      = nullptr;
    ID3D11InputLayout*       m_layout  = nullptr;
    ID3D11Buffer*            m_cb      = nullptr; // VP matrix
    ID3D11Buffer*            m_vb      = nullptr; // dynamic, 6 verts per quad
    ID3D11SamplerState*      m_sampler = nullptr;
    ID3D11BlendState*        m_blend   = nullptr;
    ID3D11RasterizerState*   m_rast    = nullptr;
    ID3D11DepthStencilState* m_ds      = nullptr;
};

} // namespace mapedit
