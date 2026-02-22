#include "primitives_3d.h"

#include <d3dcompiler.h>
#include <glog/logging.h>

#include <cmath>
#include <cstring>

#pragma comment(lib, "d3dcompiler.lib")

namespace mapedit {

// ---- HLSL shaders -------------------------------------------------------

static const char kPrimsVS[] = R"(
cbuffer CB : register(b0) {
    float4x4 viewProj;
};
struct VS_IN {
    float3 pos : POSITION;
    float4 col : COLOR;
};
struct VS_OUT {
    float4 pos : SV_POSITION;
    float4 col : COLOR;
};
VS_OUT main(VS_IN i) {
    VS_OUT o;
    o.pos = mul(float4(i.pos, 1.0), viewProj);
    o.col = i.col;
    return o;
}
)";

static const char kPrimsPS[] = R"(
struct PS_IN {
    float4 pos : SV_POSITION;
    float4 col : COLOR;
};
float4 main(PS_IN i) : SV_TARGET {
    return i.col;
}
)";

// ---- Initialize ---------------------------------------------------------

bool Primitives3D::Initialize(ID3D11Device* device, ID3D11DeviceContext* context) {
    m_device  = device;
    m_context = context;

    HRESULT hr;
    ID3DBlob* vsBlob = nullptr;
    ID3DBlob* psBlob = nullptr;
    ID3DBlob* err    = nullptr;

    // Compile vertex shader
    hr = D3DCompile(kPrimsVS, strlen(kPrimsVS), "Primitives3DVS", nullptr, nullptr,
                    "main", "vs_4_0", 0, 0, &vsBlob, &err);
    if (FAILED(hr)) {
        if (err) {
            LOG(ERROR) << "[Primitives3D] VS compile error: "
                       << reinterpret_cast<const char*>(err->GetBufferPointer());
            err->Release();
        }
        return false;
    }

    // Compile pixel shader
    hr = D3DCompile(kPrimsPS, strlen(kPrimsPS), "Primitives3DPS", nullptr, nullptr,
                    "main", "ps_4_0", 0, 0, &psBlob, &err);
    if (FAILED(hr)) {
        if (err) {
            LOG(ERROR) << "[Primitives3D] PS compile error: "
                       << reinterpret_cast<const char*>(err->GetBufferPointer());
            err->Release();
        }
        vsBlob->Release();
        return false;
    }

    // Create vertex shader
    hr = device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(),
                                    nullptr, &m_vs);
    if (FAILED(hr)) {
        LOG(ERROR) << "[Primitives3D] CreateVertexShader failed: 0x" << std::hex << hr;
        vsBlob->Release();
        psBlob->Release();
        return false;
    }

    // Create pixel shader
    hr = device->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(),
                                   nullptr, &m_ps);
    if (FAILED(hr)) {
        LOG(ERROR) << "[Primitives3D] CreatePixelShader failed: 0x" << std::hex << hr;
        vsBlob->Release();
        psBlob->Release();
        Shutdown();
        return false;
    }

    // Input layout:
    //   float3 POSITION at offset 0  (12 bytes)
    //   UNORM  COLOR    at offset 12 ( 4 bytes, auto-converted to float4 by GPU)
    D3D11_INPUT_ELEMENT_DESC layout[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT,    0,  0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "COLOR",    0, DXGI_FORMAT_R8G8B8A8_UNORM,     0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0 },
    };
    hr = device->CreateInputLayout(layout, 2,
                                   vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(),
                                   &m_layout);
    vsBlob->Release();
    psBlob->Release();
    if (FAILED(hr)) {
        LOG(ERROR) << "[Primitives3D] CreateInputLayout failed: 0x" << std::hex << hr;
        Shutdown();
        return false;
    }

    // Dynamic vertex buffer
    D3D11_BUFFER_DESC vbd = {};
    vbd.ByteWidth      = kMaxVertices * sizeof(LineVertex);
    vbd.Usage          = D3D11_USAGE_DYNAMIC;
    vbd.BindFlags      = D3D11_BIND_VERTEX_BUFFER;
    vbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    hr = device->CreateBuffer(&vbd, nullptr, &m_vb);
    if (FAILED(hr)) {
        LOG(ERROR) << "[Primitives3D] CreateBuffer (VB) failed: 0x" << std::hex << hr;
        Shutdown();
        return false;
    }

    // Constant buffer: 64-byte float4x4
    D3D11_BUFFER_DESC cbd = {};
    cbd.ByteWidth      = 64; // sizeof(float4x4)
    cbd.Usage          = D3D11_USAGE_DYNAMIC;
    cbd.BindFlags      = D3D11_BIND_CONSTANT_BUFFER;
    cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    hr = device->CreateBuffer(&cbd, nullptr, &m_cb);
    if (FAILED(hr)) {
        LOG(ERROR) << "[Primitives3D] CreateBuffer (CB) failed: 0x" << std::hex << hr;
        Shutdown();
        return false;
    }

    // Blend: standard alpha blending
    D3D11_BLEND_DESC bd = {};
    bd.RenderTarget[0].BlendEnable           = TRUE;
    bd.RenderTarget[0].SrcBlend              = D3D11_BLEND_SRC_ALPHA;
    bd.RenderTarget[0].DestBlend             = D3D11_BLEND_INV_SRC_ALPHA;
    bd.RenderTarget[0].BlendOp               = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].SrcBlendAlpha         = D3D11_BLEND_ONE;
    bd.RenderTarget[0].DestBlendAlpha        = D3D11_BLEND_INV_SRC_ALPHA;
    bd.RenderTarget[0].BlendOpAlpha          = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    hr = device->CreateBlendState(&bd, &m_blendState);
    if (FAILED(hr)) {
        LOG(ERROR) << "[Primitives3D] CreateBlendState failed: 0x" << std::hex << hr;
        Shutdown();
        return false;
    }

    // Rasterizer: no cull, no scissor, depth clip enabled, wireframe not needed
    D3D11_RASTERIZER_DESC rd = {};
    rd.FillMode        = D3D11_FILL_SOLID;
    rd.CullMode        = D3D11_CULL_NONE;
    rd.ScissorEnable   = FALSE;
    rd.DepthClipEnable = TRUE;
    hr = device->CreateRasterizerState(&rd, &m_rastState);
    if (FAILED(hr)) {
        LOG(ERROR) << "[Primitives3D] CreateRasterizerState failed: 0x" << std::hex << hr;
        Shutdown();
        return false;
    }

    // Depth-stencil: test enabled with LESS_EQUAL so lines at same depth as navmesh render on top
    D3D11_DEPTH_STENCIL_DESC dd = {};
    dd.DepthEnable    = TRUE;
    dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    dd.DepthFunc      = D3D11_COMPARISON_LESS_EQUAL;
    dd.StencilEnable  = FALSE;
    hr = device->CreateDepthStencilState(&dd, &m_dsState);
    if (FAILED(hr)) {
        LOG(ERROR) << "[Primitives3D] CreateDepthStencilState failed: 0x" << std::hex << hr;
        Shutdown();
        return false;
    }

    m_lines.reserve(4096);
    LOG(INFO) << "[Primitives3D] Initialized (max " << kMaxVertices << " vertices)";
    return true;
}

// ---- Shutdown -----------------------------------------------------------

void Primitives3D::Shutdown() {
    if (m_dsState)    { m_dsState->Release();    m_dsState    = nullptr; }
    if (m_rastState)  { m_rastState->Release();  m_rastState  = nullptr; }
    if (m_blendState) { m_blendState->Release(); m_blendState = nullptr; }
    if (m_cb)         { m_cb->Release();         m_cb         = nullptr; }
    if (m_vb)         { m_vb->Release();         m_vb         = nullptr; }
    if (m_layout)     { m_layout->Release();     m_layout     = nullptr; }
    if (m_ps)         { m_ps->Release();         m_ps         = nullptr; }
    if (m_vs)         { m_vs->Release();         m_vs         = nullptr; }
    m_device  = nullptr;
    m_context = nullptr;
}

// ---- BeginFrame ---------------------------------------------------------

void Primitives3D::BeginFrame() {
    m_lines.clear();
}

// ---- AddLine ------------------------------------------------------------

void Primitives3D::AddLine(float x1, float y1, float z1,
                            float x2, float y2, float z2,
                            uint32_t abgrColor) {
    if (m_lines.size() + 2 > kMaxVertices)
        return; // budget exhausted for this frame

    m_lines.push_back({ x1, y1, z1, abgrColor });
    m_lines.push_back({ x2, y2, z2, abgrColor });
}

// ---- AddCircle ----------------------------------------------------------

void Primitives3D::AddCircle(float cx, float cy, float cz,
                              float radius, uint32_t abgrColor, int segments) {
    if (segments < 3) segments = 3;
    if (m_lines.size() + static_cast<size_t>(segments) * 2 > kMaxVertices)
        return;

    const float step = 2.0f * 3.14159265358979f / static_cast<float>(segments);
    for (int i = 0; i < segments; ++i) {
        float a0 = step * static_cast<float>(i);
        float a1 = step * static_cast<float>(i + 1);
        float x0 = cx + radius * std::cos(a0);
        float y0 = cy + radius * std::sin(a0);
        float x1 = cx + radius * std::cos(a1);
        float y1 = cy + radius * std::sin(a1);
        m_lines.push_back({ x0, y0, cz, abgrColor });
        m_lines.push_back({ x1, y1, cz, abgrColor });
    }
}

// ---- Flush --------------------------------------------------------------

void Primitives3D::Flush(const float viewProj[16]) {
    if (!m_device || !m_context || m_lines.empty())
        return;

    // Clamp to buffer capacity
    UINT count = static_cast<UINT>(m_lines.size());
    if (count > kMaxVertices) count = kMaxVertices;

    // Update constant buffer with VP matrix
    {
        D3D11_MAPPED_SUBRESOURCE mapped = {};
        HRESULT hr = m_context->Map(m_cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
        if (FAILED(hr)) {
            LOG(WARNING) << "[Primitives3D] Map CB failed: 0x" << std::hex << hr;
            return;
        }
        std::memcpy(mapped.pData, viewProj, 64);
        m_context->Unmap(m_cb, 0);
    }

    // Upload vertices
    {
        D3D11_MAPPED_SUBRESOURCE mapped = {};
        HRESULT hr = m_context->Map(m_vb, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
        if (FAILED(hr)) {
            LOG(WARNING) << "[Primitives3D] Map VB failed: 0x" << std::hex << hr;
            return;
        }
        std::memcpy(mapped.pData, m_lines.data(), count * sizeof(LineVertex));
        m_context->Unmap(m_vb, 0);
    }

    // Bind pipeline state
    m_context->VSSetShader(m_vs, nullptr, 0);
    m_context->PSSetShader(m_ps, nullptr, 0);
    m_context->IASetInputLayout(m_layout);
    m_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_LINELIST);

    UINT stride = sizeof(LineVertex);
    UINT offset = 0;
    m_context->IASetVertexBuffers(0, 1, &m_vb, &stride, &offset);
    m_context->VSSetConstantBuffers(0, 1, &m_cb);

    float blendFactor[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    m_context->OMSetBlendState(m_blendState, blendFactor, 0xFFFFFFFF);
    m_context->OMSetDepthStencilState(m_dsState, 0);
    m_context->RSSetState(m_rastState);

    m_context->Draw(count, 0);
}

} // namespace mapedit
