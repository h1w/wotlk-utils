#include "terrain_texture_pipeline.h"
#include "terrain_pipeline.h"   // TerrainCB

#include <d3dcompiler.h>
#include <glog/logging.h>

#include <cstring>

namespace mapedit {

// ---------------------------------------------------------------------------
// Shaders — same CB layout as TerrainPipeline, but VS passes UV through
// and PS can sample a texture atlas when colorMode == 3.
// ---------------------------------------------------------------------------

static const char kTexTerrainVS[] = R"(
cbuffer TerrainCB : register(b0) {
    float4x4 viewProj;
    float4 lightDir;      // xyz=direction, w=ambient
    float4 baseColor;     // RGBA
    float4 heightParams;  // x=minZ, y=maxZ, z=colorMode
    float4 tileParams;    // x=textureSlot
};

struct VS_IN  { float3 pos : POSITION; float3 norm : NORMAL; float2 uv : TEXCOORD0; float slotIdx : TEXCOORD1; };
struct VS_OUT {
    float4 clipPos  : SV_POSITION;
    float3 normal   : TEXCOORD0;
    float3 worldPos : TEXCOORD1;
    float2 uv       : TEXCOORD2;
    float slotIdx   : TEXCOORD3;
};

VS_OUT main(VS_IN i) {
    VS_OUT o;
    float3 pos = i.pos;
    // Slope-dependent Z offset (same as procedural terrain)
    float slope = 1.0 - abs(i.norm.z);
    pos.z += baseColor.a * (1.0 + slope * 5.0);
    o.clipPos  = mul(float4(pos, 1.0), viewProj);
    o.normal   = i.norm;
    o.worldPos = i.pos;
    o.uv       = i.uv;
    o.slotIdx  = i.slotIdx;
    return o;
}
)";

static const char kTexTerrainPS[] = R"(
cbuffer TerrainCB : register(b0) {
    float4x4 viewProj;
    float4 lightDir;
    float4 baseColor;
    float4 heightParams;
    float4 tileParams;
};

Texture2DArray texAtlas : register(t0);
SamplerState   samLinear : register(s0);

struct PS_IN {
    float4 clipPos  : SV_POSITION;
    float3 normal   : TEXCOORD0;
    float3 worldPos : TEXCOORD1;
    float2 uv       : TEXCOORD2;
    float slotIdx   : TEXCOORD3;
};

float4 main(PS_IN i) : SV_TARGET {
    // Normal selection: heightParams.w == -1 -> smooth per-vertex, else derivative
    float3 N;
    if (heightParams.w > -1.5 && heightParams.w < -0.5) {
        N = normalize(i.normal);
    } else {
        float3 dpdx = ddx(i.worldPos);
        float3 dpdy = ddy(i.worldPos);
        N = -normalize(cross(dpdx, dpdy));
    }

    // Directional light
    float3 L = normalize(lightDir.xyz);
    float NdotL = dot(N, L);
    if (NdotL < 0.0) { N = -N; NdotL = -NdotL; }

    float ambient = lightDir.w;
    float lighting = ambient + (1.0 - ambient) * NdotL;

    // Color mode selection
    float3 color;
    int mode = (int)heightParams.z;

    if (mode == 3) {
        // Texture mode: sample atlas
        color = texAtlas.Sample(samLinear, float3(i.uv, i.slotIdx)).rgb;
    }
    else if (mode == 1) {
        float t = saturate((i.worldPos.z - heightParams.x) /
                           max(heightParams.y - heightParams.x, 0.01));
        color = lerp(float3(0.25, 0.25, 0.28), float3(0.65, 0.65, 0.68), t);
    }
    else if (mode == 2) {
        float slope = 1.0 - abs(N.z);
        color = lerp(float3(0.5, 0.5, 0.53), float3(0.2, 0.2, 0.22), slope);
    }
    else {
        color = baseColor.rgb;
    }

    return float4(color * lighting, 1.0);
}
)";

// ---------------------------------------------------------------------------
// Initialize
// ---------------------------------------------------------------------------

bool TerrainTexturePipeline::Initialize(ID3D11Device* device) {
    HRESULT hr;
    ID3DBlob* vsBlob = nullptr;
    ID3DBlob* psBlob = nullptr;
    ID3DBlob* err    = nullptr;

    // Compile vertex shader
    hr = D3DCompile(kTexTerrainVS, strlen(kTexTerrainVS), "TexTerrainVS", nullptr, nullptr,
                    "main", "vs_4_0", 0, 0, &vsBlob, &err);
    if (FAILED(hr)) {
        if (err) {
            LOG(ERROR) << "[TerrainTexturePipeline] VS: " << (const char*)err->GetBufferPointer();
            err->Release();
        }
        return false;
    }

    hr = D3DCompile(kTexTerrainPS, strlen(kTexTerrainPS), "TexTerrainPS", nullptr, nullptr,
                    "main", "ps_4_0", 0, 0, &psBlob, &err);
    if (FAILED(hr)) {
        if (err) {
            LOG(ERROR) << "[TerrainTexturePipeline] PS: " << (const char*)err->GetBufferPointer();
            err->Release();
        }
        vsBlob->Release();
        return false;
    }

    hr = device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(),
                                    nullptr, &m_vs);
    if (FAILED(hr)) {
        LOG(ERROR) << "[TerrainTexturePipeline] CreateVertexShader failed: 0x" << std::hex << hr;
        vsBlob->Release(); psBlob->Release();
        return false;
    }

    hr = device->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(),
                                   nullptr, &m_ps);
    if (FAILED(hr)) {
        LOG(ERROR) << "[TerrainTexturePipeline] CreatePixelShader failed: 0x" << std::hex << hr;
        vsBlob->Release(); psBlob->Release();
        Shutdown();
        return false;
    }

    // Input layout: POSITION(0) + NORMAL(12) + TEXCOORD0(24) + TEXCOORD1(32)
    D3D11_INPUT_ELEMENT_DESC layout[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0,  0, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"NORMAL",   0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,    0, 24, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 1, DXGI_FORMAT_R32_FLOAT,       0, 32, D3D11_INPUT_PER_VERTEX_DATA, 0},
    };
    hr = device->CreateInputLayout(layout, 4,
                                   vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(),
                                   &m_layout);
    vsBlob->Release();
    psBlob->Release();
    if (FAILED(hr)) {
        LOG(ERROR) << "[TerrainTexturePipeline] CreateInputLayout failed: 0x" << std::hex << hr;
        Shutdown();
        return false;
    }

    // Blend: opaque
    D3D11_BLEND_DESC bd = {};
    bd.RenderTarget[0].BlendEnable           = FALSE;
    bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    hr = device->CreateBlendState(&bd, &m_blendState);
    if (FAILED(hr)) { Shutdown(); return false; }

    // Rasterizer: solid, back-face cull
    D3D11_RASTERIZER_DESC rd = {};
    rd.FillMode        = D3D11_FILL_SOLID;
    rd.CullMode        = D3D11_CULL_BACK;
    rd.FrontCounterClockwise = FALSE;
    rd.DepthClipEnable = TRUE;
    hr = device->CreateRasterizerState(&rd, &m_rastState);
    if (FAILED(hr)) { Shutdown(); return false; }

    // Depth: LESS, write ON
    D3D11_DEPTH_STENCIL_DESC dd = {};
    dd.DepthEnable    = TRUE;
    dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    dd.DepthFunc      = D3D11_COMPARISON_LESS;
    dd.StencilEnable  = FALSE;
    hr = device->CreateDepthStencilState(&dd, &m_dsState);
    if (FAILED(hr)) { Shutdown(); return false; }

    // Constant buffer
    D3D11_BUFFER_DESC cbd = {};
    cbd.ByteWidth      = sizeof(TerrainCB);
    cbd.Usage          = D3D11_USAGE_DYNAMIC;
    cbd.BindFlags      = D3D11_BIND_CONSTANT_BUFFER;
    cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    hr = device->CreateBuffer(&cbd, nullptr, &m_cb);
    if (FAILED(hr)) { Shutdown(); return false; }

    // Sampler: anisotropic 8x, clamp
    D3D11_SAMPLER_DESC sd = {};
    sd.Filter   = D3D11_FILTER_ANISOTROPIC;
    sd.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxAnisotropy = 8;
    sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    hr = device->CreateSamplerState(&sd, &m_sampler);
    if (FAILED(hr)) { Shutdown(); return false; }

    LOG(INFO) << "[TerrainTexturePipeline] Initialized";
    return true;
}

void TerrainTexturePipeline::Shutdown() {
    if (m_sampler)    { m_sampler->Release();    m_sampler = nullptr; }
    if (m_cb)         { m_cb->Release();         m_cb = nullptr; }
    if (m_dsState)    { m_dsState->Release();    m_dsState = nullptr; }
    if (m_rastState)  { m_rastState->Release();  m_rastState = nullptr; }
    if (m_blendState) { m_blendState->Release(); m_blendState = nullptr; }
    if (m_layout)     { m_layout->Release();     m_layout = nullptr; }
    if (m_ps)         { m_ps->Release();         m_ps = nullptr; }
    if (m_vs)         { m_vs->Release();         m_vs = nullptr; }
}

} // namespace mapedit
