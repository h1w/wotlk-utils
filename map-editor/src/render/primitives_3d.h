#pragma once
#include <d3d11.h>
#include <cstdint>
#include <vector>

namespace mapedit {

// Utility for drawing 3D lines and circles. Accumulates geometry per frame
// and flushes in a single draw call.
class Primitives3D {
public:
    bool Initialize(ID3D11Device* device, ID3D11DeviceContext* context);
    void Shutdown();

    // Reset dynamic buffer for new frame
    void BeginFrame();

    // Add a line segment (world coords)
    void AddLine(float x1, float y1, float z1,
                 float x2, float y2, float z2,
                 uint32_t abgrColor);

    // Add a circle (flat on XY plane) at world position
    void AddCircle(float cx, float cy, float cz,
                   float radius, uint32_t abgrColor, int segments = 16);

    // Flush accumulated geometry with given VP matrix (row-major float[16])
    void Flush(const float viewProj[16]);

private:
    struct LineVertex {
        float    x, y, z;
        uint32_t color; // ABGR packed, interpreted as DXGI_FORMAT_R8G8B8A8_UNORM
    };

    ID3D11Device*            m_device    = nullptr;
    ID3D11DeviceContext*     m_context   = nullptr;
    ID3D11Buffer*            m_vb        = nullptr; // dynamic VB
    ID3D11Buffer*            m_cb        = nullptr; // VP matrix CB
    ID3D11VertexShader*      m_vs        = nullptr;
    ID3D11PixelShader*       m_ps        = nullptr;
    ID3D11InputLayout*       m_layout    = nullptr;
    ID3D11DepthStencilState* m_dsState   = nullptr;
    ID3D11BlendState*        m_blendState = nullptr;
    ID3D11RasterizerState*   m_rastState  = nullptr;

    std::vector<LineVertex> m_lines; // accumulated this frame
    static constexpr UINT kMaxVertices = 65536;
};

} // namespace mapedit
