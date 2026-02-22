#include "navmesh_pipeline_3d.h"

#include <d3dcompiler.h>
#include <glog/logging.h>

#include <cstring>

namespace mapedit {

static const char kVS3D[] = R"(
cbuffer NavmeshCB3D : register(b0) {
    float4x4 viewProj;
    float4 fillColor;
    float4 edgeColor;
    float4 lightDir;
    float4 colorParams;  // [0]=colorMode, [1]=heightMin, [2]=heightMax, [3]=drawEdges
    float4 tileHue;      // [0]=tile hue (0-1)
};
struct VS_IN  { float3 pos : POSITION; float3 bary : TEXCOORD0; };
struct VS_OUT {
    float4 clipPos  : SV_POSITION;
    float3 bary     : TEXCOORD0;
    float3 worldPos : TEXCOORD1;
};
VS_OUT main(VS_IN i) {
    VS_OUT o;
    o.clipPos  = mul(float4(i.pos, 1.0), viewProj);
    o.bary     = i.bary;
    o.worldPos = i.pos;
    return o;
}
)";

static const char kPS3D[] = R"(
cbuffer NavmeshCB3D : register(b0) {
    float4x4 viewProj;
    float4 fillColor;
    float4 edgeColor;
    float4 lightDir;
    float4 colorParams;  // [0]=colorMode, [1]=heightMin, [2]=heightMax, [3]=drawEdges
    float4 tileHue;      // [0]=tile hue (0-1)
};

// HSV to RGB conversion (S and V in 0-1, H in 0-1)
float3 HSVtoRGB(float3 hsv) {
    float h = hsv.x * 6.0;
    float s = hsv.y;
    float v = hsv.z;
    float c = v * s;
    float x = c * (1.0 - abs(fmod(h, 2.0) - 1.0));
    float m = v - c;
    float3 rgb;
    if      (h < 1.0) rgb = float3(c, x, 0);
    else if (h < 2.0) rgb = float3(x, c, 0);
    else if (h < 3.0) rgb = float3(0, c, x);
    else if (h < 4.0) rgb = float3(0, x, c);
    else if (h < 5.0) rgb = float3(x, 0, c);
    else              rgb = float3(c, 0, x);
    return rgb + float3(m, m, m);
}

struct PS_IN {
    float4 clipPos  : SV_POSITION;
    float3 bary     : TEXCOORD0;
    float3 worldPos : TEXCOORD1;
};
float4 main(PS_IN i) : SV_TARGET {
    // Flat normal from derivatives (needed for lighting and slope mode)
    // NOTE: clip-space X-flip negates ddx, which flips the cross product.
    // Negate to get the correct outward-facing normal.
    float3 dpdx = ddx(i.worldPos);
    float3 dpdy = ddy(i.worldPos);
    float3 N = -normalize(cross(dpdx, dpdy));

    int mode = (int)colorParams.x;

    // Compute base solid color depending on color mode
    float3 solidColor;
    float  solidAlpha;

    if (mode == 1) {
        // HeightGradient: Blue(low) -> Green(mid) -> White(high)
        float heightNorm = saturate((i.worldPos.z - colorParams.y) / (colorParams.z - colorParams.y + 0.001));
        if (heightNorm < 0.5) {
            solidColor = lerp(float3(0.0, 0.2, 0.6), float3(0.0, 0.7, 0.3), heightNorm * 2.0);
        } else {
            solidColor = lerp(float3(0.0, 0.7, 0.3), float3(0.95, 0.97, 1.0), (heightNorm - 0.5) * 2.0);
        }
        solidAlpha = fillColor.a;
    } else if (mode == 2) {
        // SlopeShading: flat=green, steep=red
        float slope = 1.0 - abs(N.z);
        solidColor = lerp(float3(0.0, 0.7, 0.3), float3(0.9, 0.2, 0.1), saturate(slope * 2.0));
        solidAlpha = fillColor.a;
    } else if (mode == 3) {
        // TileColored: HSV with fixed S=0.5, V=0.8
        solidColor = HSVtoRGB(float3(tileHue.x, 0.5, 0.8));
        solidAlpha = fillColor.a;
    } else {
        // FlatGreen (mode 0): use fillColor directly
        solidColor = fillColor.rgb;
        solidAlpha = fillColor.a;
    }

    // Wireframe edge blending (only when drawEdges is enabled)
    float4 baseColor;
    if (colorParams.w > 0.5) {
        float3 d = fwidth(i.bary);
        float3 s = smoothstep(float3(0,0,0), d * 1.5, i.bary);
        float  w = min(s.x, min(s.y, s.z));
        // For non-FlatGreen modes, derive edge color as brightened solid color
        float3 eColor;
        if (mode == 0) {
            eColor = edgeColor.rgb;
        } else {
            eColor = saturate(solidColor * 1.3 + float3(0.1, 0.1, 0.1));
        }
        baseColor = float4(lerp(eColor, solidColor, w), solidAlpha);
    } else {
        // No wireframe — solid fill only
        baseColor = float4(solidColor, solidAlpha);
    }

    // Simple directional light
    float NdotL = max(dot(N, normalize(lightDir.xyz)), 0.0);
    float ambient = 0.3;
    float lighting = ambient + (1.0 - ambient) * NdotL;

    return float4(baseColor.rgb * lighting, baseColor.a);
}
)";

bool NavmeshPipeline3D::Initialize(ID3D11Device* device) {
    HRESULT hr;
    ID3DBlob* vsBlob = nullptr;
    ID3DBlob* psBlob = nullptr;
    ID3DBlob* err    = nullptr;

    // Compile vertex shader
    hr = D3DCompile(kVS3D, strlen(kVS3D), "NavmeshVS3D", nullptr, nullptr,
                    "main", "vs_4_0", 0, 0, &vsBlob, &err);
    if (FAILED(hr)) {
        if (err) {
            LOG(ERROR) << "[NavmeshPipeline3D] VS: " << (const char*)err->GetBufferPointer();
            err->Release();
        }
        return false;
    }

    // Compile pixel shader
    hr = D3DCompile(kPS3D, strlen(kPS3D), "NavmeshPS3D", nullptr, nullptr,
                    "main", "ps_4_0", 0, 0, &psBlob, &err);
    if (FAILED(hr)) {
        if (err) {
            LOG(ERROR) << "[NavmeshPipeline3D] PS: " << (const char*)err->GetBufferPointer();
            err->Release();
        }
        vsBlob->Release();
        return false;
    }

    // Create vertex shader
    hr = device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(),
                                    nullptr, &m_vs);
    if (FAILED(hr)) {
        LOG(ERROR) << "[NavmeshPipeline3D] CreateVertexShader failed: 0x" << std::hex << hr;
        vsBlob->Release();
        psBlob->Release();
        return false;
    }

    // Create pixel shader
    hr = device->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(),
                                   nullptr, &m_ps);
    if (FAILED(hr)) {
        LOG(ERROR) << "[NavmeshPipeline3D] CreatePixelShader failed: 0x" << std::hex << hr;
        vsBlob->Release();
        psBlob->Release();
        Shutdown();
        return false;
    }

    // Input layout: float3 POSITION (offset 0, 12 bytes) + float3 TEXCOORD0 (offset 12, 12 bytes)
    D3D11_INPUT_ELEMENT_DESC layout[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0,  0, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0},
    };
    hr = device->CreateInputLayout(layout, 2,
                                   vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(),
                                   &m_inputLayout);
    vsBlob->Release();
    psBlob->Release();
    if (FAILED(hr)) {
        LOG(ERROR) << "[NavmeshPipeline3D] CreateInputLayout failed: 0x" << std::hex << hr;
        Shutdown();
        return false;
    }

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
    if (FAILED(hr)) {
        LOG(ERROR) << "[NavmeshPipeline3D] CreateBlendState failed: 0x" << std::hex << hr;
        Shutdown();
        return false;
    }

    // Rasterizer: solid fill, no cull, depth clip enabled, no scissor
    D3D11_RASTERIZER_DESC rd = {};
    rd.FillMode        = D3D11_FILL_SOLID;
    rd.CullMode        = D3D11_CULL_NONE;
    rd.ScissorEnable   = FALSE;
    rd.DepthClipEnable = TRUE;
    hr = device->CreateRasterizerState(&rd, &m_rastState);
    if (FAILED(hr)) {
        LOG(ERROR) << "[NavmeshPipeline3D] CreateRasterizerState failed: 0x" << std::hex << hr;
        Shutdown();
        return false;
    }

    // Depth-stencil: depth test enabled, write OFF (transparent overlay), LESS_EQUAL
    D3D11_DEPTH_STENCIL_DESC dd = {};
    dd.DepthEnable    = TRUE;
    dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    dd.DepthFunc      = D3D11_COMPARISON_LESS_EQUAL;
    dd.StencilEnable  = FALSE;
    hr = device->CreateDepthStencilState(&dd, &m_dsState);
    if (FAILED(hr)) {
        LOG(ERROR) << "[NavmeshPipeline3D] CreateDepthStencilState failed: 0x" << std::hex << hr;
        Shutdown();
        return false;
    }

    // Dynamic constant buffer: sizeof(NavmeshCB3D) = 160 bytes
    D3D11_BUFFER_DESC cbd = {};
    cbd.ByteWidth      = sizeof(NavmeshCB3D);
    cbd.Usage          = D3D11_USAGE_DYNAMIC;
    cbd.BindFlags      = D3D11_BIND_CONSTANT_BUFFER;
    cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    hr = device->CreateBuffer(&cbd, nullptr, &m_cb);
    if (FAILED(hr)) {
        LOG(ERROR) << "[NavmeshPipeline3D] CreateBuffer (CB) failed: 0x" << std::hex << hr;
        Shutdown();
        return false;
    }

    LOG(INFO) << "[NavmeshPipeline3D] Initialized";
    return true;
}

void NavmeshPipeline3D::Shutdown() {
    if (m_cb)         { m_cb->Release();         m_cb = nullptr; }
    if (m_dsState)    { m_dsState->Release();    m_dsState = nullptr; }
    if (m_rastState)  { m_rastState->Release();  m_rastState = nullptr; }
    if (m_blendState) { m_blendState->Release(); m_blendState = nullptr; }
    if (m_inputLayout){ m_inputLayout->Release(); m_inputLayout = nullptr; }
    if (m_ps)         { m_ps->Release();         m_ps = nullptr; }
    if (m_vs)         { m_vs->Release();         m_vs = nullptr; }
}

} // namespace mapedit
