#include "navmesh_pipeline.h"

#include <d3dcompiler.h>
#include <glog/logging.h>

#include <cstring>

namespace mapedit {

static const char kVS[] = R"(
cbuffer NavmeshCB : register(b0) {
    float4 transform;   // (viewCenterX, viewCenterY, scaleX, scaleY)
    float4 fillColor;
    float4 edgeColor;
};
struct VS_IN  { float2 pos : POSITION; float3 bary : TEXCOORD0; };
struct VS_OUT { float4 pos : SV_POSITION; float3 bary : TEXCOORD0; };
VS_OUT main(VS_IN i) {
    VS_OUT o;
    o.pos.x =  (transform.y - i.pos.y) * transform.z;
    o.pos.y = -(transform.x - i.pos.x) * transform.w;
    o.pos.z = 0.5;
    o.pos.w = 1.0;
    o.bary  = i.bary;
    return o;
}
)";

static const char kPS[] = R"(
cbuffer NavmeshCB : register(b0) {
    float4 transform;
    float4 fillColor;
    float4 edgeColor;
};
struct PS_IN { float4 pos : SV_POSITION; float3 bary : TEXCOORD0; };
float4 main(PS_IN i) : SV_TARGET {
    float3 d = fwidth(i.bary);
    float3 s = smoothstep(float3(0,0,0), d * 1.5, i.bary);
    float  w = min(s.x, min(s.y, s.z));
    return lerp(edgeColor, fillColor, w);
}
)";

bool NavmeshPipeline::Initialize(ID3D11Device* device) {
    HRESULT hr;
    ID3DBlob* vsBlob = nullptr;
    ID3DBlob* psBlob = nullptr;
    ID3DBlob* err = nullptr;

    // Compile vertex shader
    hr = D3DCompile(kVS, strlen(kVS), "NavmeshVS", nullptr, nullptr,
                    "main", "vs_4_0", 0, 0, &vsBlob, &err);
    if (FAILED(hr)) {
        if (err) {
            LOG(ERROR) << "[NavmeshPipeline] VS: " << (const char*)err->GetBufferPointer();
            err->Release();
        }
        return false;
    }

    // Compile pixel shader
    hr = D3DCompile(kPS, strlen(kPS), "NavmeshPS", nullptr, nullptr,
                    "main", "ps_4_0", 0, 0, &psBlob, &err);
    if (FAILED(hr)) {
        if (err) {
            LOG(ERROR) << "[NavmeshPipeline] PS: " << (const char*)err->GetBufferPointer();
            err->Release();
        }
        vsBlob->Release();
        return false;
    }

    // Create shaders
    hr = device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(),
                                    nullptr, &m_vs);
    if (FAILED(hr)) { vsBlob->Release(); psBlob->Release(); return false; }

    hr = device->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(),
                                   nullptr, &m_ps);
    if (FAILED(hr)) { vsBlob->Release(); psBlob->Release(); Shutdown(); return false; }

    // Input layout: float2 POSITION + float3 TEXCOORD0
    D3D11_INPUT_ELEMENT_DESC layout[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT,    0, 0,  D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 8,  D3D11_INPUT_PER_VERTEX_DATA, 0},
    };
    hr = device->CreateInputLayout(layout, 2,
                                   vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(),
                                   &m_inputLayout);
    vsBlob->Release();
    psBlob->Release();
    if (FAILED(hr)) { Shutdown(); return false; }

    // Blend: standard alpha
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
    if (FAILED(hr)) { Shutdown(); return false; }

    // Rasterizer: fill, no cull, scissor
    D3D11_RASTERIZER_DESC rd = {};
    rd.FillMode        = D3D11_FILL_SOLID;
    rd.CullMode        = D3D11_CULL_NONE;
    rd.ScissorEnable   = TRUE;
    rd.DepthClipEnable = FALSE;
    hr = device->CreateRasterizerState(&rd, &m_rastState);
    if (FAILED(hr)) { Shutdown(); return false; }

    // Depth-stencil: disabled
    D3D11_DEPTH_STENCIL_DESC dd = {};
    dd.DepthEnable   = FALSE;
    dd.StencilEnable = FALSE;
    hr = device->CreateDepthStencilState(&dd, &m_dsState);
    if (FAILED(hr)) { Shutdown(); return false; }

    // Dynamic constant buffer
    D3D11_BUFFER_DESC cbd = {};
    cbd.ByteWidth      = sizeof(NavmeshCB);
    cbd.Usage           = D3D11_USAGE_DYNAMIC;
    cbd.BindFlags       = D3D11_BIND_CONSTANT_BUFFER;
    cbd.CPUAccessFlags  = D3D11_CPU_ACCESS_WRITE;
    hr = device->CreateBuffer(&cbd, nullptr, &m_cb);
    if (FAILED(hr)) { Shutdown(); return false; }

    LOG(INFO) << "[NavmeshPipeline] Initialized";
    return true;
}

void NavmeshPipeline::Shutdown() {
    if (m_cb)        { m_cb->Release();        m_cb = nullptr; }
    if (m_dsState)   { m_dsState->Release();   m_dsState = nullptr; }
    if (m_rastState) { m_rastState->Release();  m_rastState = nullptr; }
    if (m_blendState){ m_blendState->Release(); m_blendState = nullptr; }
    if (m_inputLayout){ m_inputLayout->Release(); m_inputLayout = nullptr; }
    if (m_ps)        { m_ps->Release();        m_ps = nullptr; }
    if (m_vs)        { m_vs->Release();        m_vs = nullptr; }
}

} // namespace mapedit
