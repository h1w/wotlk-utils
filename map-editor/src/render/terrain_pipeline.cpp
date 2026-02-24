#include "terrain_pipeline.h"

#include <d3dcompiler.h>
#include <glog/logging.h>

#include <cstring>

namespace mapedit {

static const char kTerrainVS[] = R"(
cbuffer TerrainCB : register(b0) {
    float4x4 viewProj;
    float4 lightDir;      // xyz=direction, w=ambient
    float4 baseColor;     // RGBA
    float4 heightParams;  // x=minZ, y=maxZ, z=colorMode
};

struct VS_IN  { float3 pos : POSITION; float3 norm : NORMAL; };
struct VS_OUT {
    float4 clipPos  : SV_POSITION;
    float3 normal   : TEXCOORD0;
    float3 worldPos : TEXCOORD1;
};

VS_OUT main(VS_IN i) {
    VS_OUT o;
    float3 pos = i.pos;
    // Slope-dependent Z offset: steeper terrain gets pushed down more.
    // baseColor.a < 0 when smooth enabled, 0 when disabled.
    float slope = 1.0 - abs(i.norm.z);   // 0=flat, 1=vertical
    pos.z += baseColor.a * (1.0 + slope * 5.0);  // flat:-1, steep:-6
    o.clipPos  = mul(float4(pos, 1.0), viewProj);
    o.normal   = i.norm;
    o.worldPos = i.pos;    // original pos for lighting
    return o;
}
)";

static const char kTerrainPS[] = R"(
cbuffer TerrainCB : register(b0) {
    float4x4 viewProj;
    float4 lightDir;
    float4 baseColor;
    float4 heightParams;
};

struct PS_IN {
    float4 clipPos  : SV_POSITION;
    float3 normal   : TEXCOORD0;
    float3 worldPos : TEXCOORD1;
};

float4 main(PS_IN i) : SV_TARGET {
    // heightParams.w == -1: terrain smooth — use per-vertex normals.
    // heightParams.w == -2: terrain flat — use derivative normals.
    // heightParams.w >= 0:  buildings — use derivative normals.
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
    // Two-sided: flip normal if facing away from light
    if (NdotL < 0.0) { N = -N; NdotL = -NdotL; }

    // Wall culling for interior groups viewed from outside (heightParams.w > 0).
    // normal.z = -1 for interior groups, 0 for exterior/alwaysdraw.
    if (heightParams.w > 0 && i.normal.z < 0) {
        if (abs(N.z) < 0.3)
            discard;
    }

    float ambient = lightDir.w;  // e.g. 0.3
    float lighting = ambient + (1.0 - ambient) * NdotL;

    // Color mode selection
    float3 color;
    int mode = (int)heightParams.z;

    if (mode == 1) {
        // Height gradient: dark grey (low) -> light grey (high)
        float t = saturate((i.worldPos.z - heightParams.x) /
                           max(heightParams.y - heightParams.x, 0.01));
        color = lerp(float3(0.25, 0.25, 0.28), float3(0.65, 0.65, 0.68), t);
    }
    else if (mode == 2) {
        // Slope shading: flat=grey, steep=dark
        float slope = 1.0 - abs(N.z);  // 0=flat, 1=vertical
        color = lerp(float3(0.5, 0.5, 0.53), float3(0.2, 0.2, 0.22), slope);
    }
    else {
        // Solid grey (mode 0, Recast Demo style)
        color = baseColor.rgb;
    }

    return float4(color * lighting, 1.0);  // fully opaque
}
)";

bool TerrainPipeline::Initialize(ID3D11Device* device) {
    HRESULT hr;
    ID3DBlob* vsBlob = nullptr;
    ID3DBlob* psBlob = nullptr;
    ID3DBlob* err    = nullptr;

    // Compile vertex shader
    hr = D3DCompile(kTerrainVS, strlen(kTerrainVS), "TerrainVS", nullptr, nullptr,
                    "main", "vs_4_0", 0, 0, &vsBlob, &err);
    if (FAILED(hr)) {
        if (err) {
            LOG(ERROR) << "[TerrainPipeline] VS: " << (const char*)err->GetBufferPointer();
            err->Release();
        }
        return false;
    }

    // Compile pixel shader
    hr = D3DCompile(kTerrainPS, strlen(kTerrainPS), "TerrainPS", nullptr, nullptr,
                    "main", "ps_4_0", 0, 0, &psBlob, &err);
    if (FAILED(hr)) {
        if (err) {
            LOG(ERROR) << "[TerrainPipeline] PS: " << (const char*)err->GetBufferPointer();
            err->Release();
        }
        vsBlob->Release();
        return false;
    }

    // Create vertex shader
    hr = device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(),
                                    nullptr, &m_vs);
    if (FAILED(hr)) {
        LOG(ERROR) << "[TerrainPipeline] CreateVertexShader failed: 0x" << std::hex << hr;
        vsBlob->Release(); psBlob->Release();
        return false;
    }

    // Create pixel shader
    hr = device->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(),
                                   nullptr, &m_ps);
    if (FAILED(hr)) {
        LOG(ERROR) << "[TerrainPipeline] CreatePixelShader failed: 0x" << std::hex << hr;
        vsBlob->Release(); psBlob->Release();
        Shutdown();
        return false;
    }

    // Input layout: float3 POSITION (offset 0) + float3 NORMAL (offset 12)
    D3D11_INPUT_ELEMENT_DESC layout[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0,  0, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"NORMAL",   0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0},
    };
    hr = device->CreateInputLayout(layout, 2,
                                   vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(),
                                   &m_layout);
    vsBlob->Release();
    psBlob->Release();
    if (FAILED(hr)) {
        LOG(ERROR) << "[TerrainPipeline] CreateInputLayout failed: 0x" << std::hex << hr;
        Shutdown();
        return false;
    }

    // Blend: OPAQUE (no blending)
    D3D11_BLEND_DESC bd = {};
    bd.RenderTarget[0].BlendEnable           = FALSE;
    bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    hr = device->CreateBlendState(&bd, &m_blendState);
    if (FAILED(hr)) {
        LOG(ERROR) << "[TerrainPipeline] CreateBlendState failed: 0x" << std::hex << hr;
        Shutdown();
        return false;
    }

    // Rasterizer: solid fill, back-face cull, depth clip enabled
    D3D11_RASTERIZER_DESC rd = {};
    rd.FillMode        = D3D11_FILL_SOLID;
    rd.CullMode        = D3D11_CULL_BACK;
    rd.FrontCounterClockwise = FALSE;
    rd.ScissorEnable   = FALSE;
    rd.DepthClipEnable = TRUE;
    hr = device->CreateRasterizerState(&rd, &m_rastState);
    if (FAILED(hr)) {
        LOG(ERROR) << "[TerrainPipeline] CreateRasterizerState failed: 0x" << std::hex << hr;
        Shutdown();
        return false;
    }

    // Depth-stencil: depth test LESS, depth write ON
    D3D11_DEPTH_STENCIL_DESC dd = {};
    dd.DepthEnable    = TRUE;
    dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    dd.DepthFunc      = D3D11_COMPARISON_LESS;
    dd.StencilEnable  = FALSE;
    hr = device->CreateDepthStencilState(&dd, &m_dsState);
    if (FAILED(hr)) {
        LOG(ERROR) << "[TerrainPipeline] CreateDepthStencilState failed: 0x" << std::hex << hr;
        Shutdown();
        return false;
    }

    // Dynamic constant buffer: sizeof(TerrainCB) = 112 bytes
    D3D11_BUFFER_DESC cbd = {};
    cbd.ByteWidth      = sizeof(TerrainCB);
    cbd.Usage          = D3D11_USAGE_DYNAMIC;
    cbd.BindFlags      = D3D11_BIND_CONSTANT_BUFFER;
    cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    hr = device->CreateBuffer(&cbd, nullptr, &m_cb);
    if (FAILED(hr)) {
        LOG(ERROR) << "[TerrainPipeline] CreateBuffer (CB) failed: 0x" << std::hex << hr;
        Shutdown();
        return false;
    }

    LOG(INFO) << "[TerrainPipeline] Initialized";
    return true;
}

void TerrainPipeline::Shutdown() {
    if (m_cb)         { m_cb->Release();         m_cb = nullptr; }
    if (m_dsState)    { m_dsState->Release();    m_dsState = nullptr; }
    if (m_rastState)  { m_rastState->Release();  m_rastState = nullptr; }
    if (m_blendState) { m_blendState->Release(); m_blendState = nullptr; }
    if (m_layout)     { m_layout->Release();     m_layout = nullptr; }
    if (m_ps)         { m_ps->Release();         m_ps = nullptr; }
    if (m_vs)         { m_vs->Release();         m_vs = nullptr; }
}

} // namespace mapedit
