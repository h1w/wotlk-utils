#include "ground_plane_3d.h"
#include "../camera/camera3d.h"
#include "minimap_cache.h"

#include <d3dcompiler.h>
#include <glog/logging.h>

#include <algorithm>
#include <cstring>

#pragma comment(lib, "d3dcompiler.lib")

namespace mapedit {

// ---------------------------------------------------------------------------
// HLSL shaders
// ---------------------------------------------------------------------------

static const char kGroundVS[] = R"(
cbuffer GroundCB : register(b0) {
    float4x4 viewProj;
};
struct VS_IN {
    float3 pos : POSITION;
    float2 uv  : TEXCOORD0;
};
struct VS_OUT {
    float4 clipPos : SV_POSITION;
    float2 uv      : TEXCOORD0;
};
VS_OUT main(VS_IN i) {
    VS_OUT o;
    o.clipPos = mul(float4(i.pos, 1.0), viewProj);
    o.uv      = i.uv;
    return o;
}
)";

static const char kGroundPS[] = R"(
Texture2D    tileTex : register(t0);
SamplerState tileSam : register(s0);

struct PS_IN {
    float4 clipPos : SV_POSITION;
    float2 uv      : TEXCOORD0;
};

float4 main(PS_IN i) : SV_TARGET {
    float4 c = tileTex.Sample(tileSam, i.uv);
    c.a = 0.4;
    return c;
}
)";

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

static constexpr float kTileSize = 533.33333f;

// Max quads per draw batch (one quad = 6 verts).
// Each tile has a unique texture so we draw one tile at a time,
// but we allocate the VB large enough for future batching.
static constexpr int kMaxQuads = 200;
static constexpr int kVertsPerQuad = 6;

// ---------------------------------------------------------------------------
// Initialize
// ---------------------------------------------------------------------------

bool GroundPlane3D::Initialize(ID3D11Device* device, ID3D11DeviceContext* context) {
    m_device  = device;
    m_context = context;

    HRESULT hr;
    ID3DBlob* vsBlob = nullptr;
    ID3DBlob* psBlob = nullptr;
    ID3DBlob* err    = nullptr;

    // Compile vertex shader
    hr = D3DCompile(kGroundVS, strlen(kGroundVS), "GroundVS", nullptr, nullptr,
                    "main", "vs_4_0", 0, 0, &vsBlob, &err);
    if (FAILED(hr)) {
        if (err) {
            LOG(ERROR) << "[GroundPlane3D] VS compile: "
                       << static_cast<const char*>(err->GetBufferPointer());
            err->Release();
        }
        return false;
    }

    // Compile pixel shader
    hr = D3DCompile(kGroundPS, strlen(kGroundPS), "GroundPS", nullptr, nullptr,
                    "main", "ps_4_0", 0, 0, &psBlob, &err);
    if (FAILED(hr)) {
        if (err) {
            LOG(ERROR) << "[GroundPlane3D] PS compile: "
                       << static_cast<const char*>(err->GetBufferPointer());
            err->Release();
        }
        vsBlob->Release();
        return false;
    }

    // Create vertex shader
    hr = device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(),
                                    nullptr, &m_vs);
    if (FAILED(hr)) {
        LOG(ERROR) << "[GroundPlane3D] CreateVertexShader failed: 0x" << std::hex << hr;
        vsBlob->Release();
        psBlob->Release();
        return false;
    }

    // Create pixel shader
    hr = device->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(),
                                   nullptr, &m_ps);
    if (FAILED(hr)) {
        LOG(ERROR) << "[GroundPlane3D] CreatePixelShader failed: 0x" << std::hex << hr;
        vsBlob->Release();
        psBlob->Release();
        Shutdown();
        return false;
    }

    // Input layout: float3 POSITION (offset 0) + float2 TEXCOORD0 (offset 12) = 20 bytes
    D3D11_INPUT_ELEMENT_DESC layout[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0,  0, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,    0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0},
    };
    hr = device->CreateInputLayout(layout, 2,
                                   vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(),
                                   &m_layout);
    vsBlob->Release();
    psBlob->Release();
    if (FAILED(hr)) {
        LOG(ERROR) << "[GroundPlane3D] CreateInputLayout failed: 0x" << std::hex << hr;
        Shutdown();
        return false;
    }

    // Dynamic vertex buffer (6 verts per quad * kMaxQuads)
    D3D11_BUFFER_DESC vbd = {};
    vbd.ByteWidth      = static_cast<UINT>(kMaxQuads * kVertsPerQuad * sizeof(QuadVertex));
    vbd.Usage          = D3D11_USAGE_DYNAMIC;
    vbd.BindFlags      = D3D11_BIND_VERTEX_BUFFER;
    vbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    hr = device->CreateBuffer(&vbd, nullptr, &m_vb);
    if (FAILED(hr)) {
        LOG(ERROR) << "[GroundPlane3D] CreateBuffer (VB) failed: 0x" << std::hex << hr;
        Shutdown();
        return false;
    }

    // Constant buffer (64 bytes = one 4x4 matrix)
    D3D11_BUFFER_DESC cbd = {};
    cbd.ByteWidth      = 64;
    cbd.Usage          = D3D11_USAGE_DYNAMIC;
    cbd.BindFlags      = D3D11_BIND_CONSTANT_BUFFER;
    cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    hr = device->CreateBuffer(&cbd, nullptr, &m_cb);
    if (FAILED(hr)) {
        LOG(ERROR) << "[GroundPlane3D] CreateBuffer (CB) failed: 0x" << std::hex << hr;
        Shutdown();
        return false;
    }

    // Linear-wrap sampler
    D3D11_SAMPLER_DESC sd = {};
    sd.Filter         = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU       = D3D11_TEXTURE_ADDRESS_WRAP;
    sd.AddressV       = D3D11_TEXTURE_ADDRESS_WRAP;
    sd.AddressW       = D3D11_TEXTURE_ADDRESS_WRAP;
    sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
    sd.MaxLOD         = D3D11_FLOAT32_MAX;
    hr = device->CreateSamplerState(&sd, &m_sampler);
    if (FAILED(hr)) {
        LOG(ERROR) << "[GroundPlane3D] CreateSamplerState failed: 0x" << std::hex << hr;
        Shutdown();
        return false;
    }

    // Alpha blend state (standard src-alpha blending)
    D3D11_BLEND_DESC bd = {};
    bd.RenderTarget[0].BlendEnable           = TRUE;
    bd.RenderTarget[0].SrcBlend              = D3D11_BLEND_SRC_ALPHA;
    bd.RenderTarget[0].DestBlend             = D3D11_BLEND_INV_SRC_ALPHA;
    bd.RenderTarget[0].BlendOp               = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].SrcBlendAlpha         = D3D11_BLEND_ONE;
    bd.RenderTarget[0].DestBlendAlpha        = D3D11_BLEND_INV_SRC_ALPHA;
    bd.RenderTarget[0].BlendOpAlpha          = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    hr = device->CreateBlendState(&bd, &m_blend);
    if (FAILED(hr)) {
        LOG(ERROR) << "[GroundPlane3D] CreateBlendState failed: 0x" << std::hex << hr;
        Shutdown();
        return false;
    }

    // Rasterizer: no cull, solid fill, depth clip
    D3D11_RASTERIZER_DESC rd = {};
    rd.FillMode        = D3D11_FILL_SOLID;
    rd.CullMode        = D3D11_CULL_NONE;
    rd.ScissorEnable   = FALSE;
    rd.DepthClipEnable = TRUE;
    hr = device->CreateRasterizerState(&rd, &m_rast);
    if (FAILED(hr)) {
        LOG(ERROR) << "[GroundPlane3D] CreateRasterizerState failed: 0x" << std::hex << hr;
        Shutdown();
        return false;
    }

    // Depth-stencil: test LESS_EQUAL, write DISABLED
    // Ground plane renders behind everything but does not occlude navmesh.
    D3D11_DEPTH_STENCIL_DESC dd = {};
    dd.DepthEnable    = TRUE;
    dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    dd.DepthFunc      = D3D11_COMPARISON_LESS_EQUAL;
    dd.StencilEnable  = FALSE;
    hr = device->CreateDepthStencilState(&dd, &m_ds);
    if (FAILED(hr)) {
        LOG(ERROR) << "[GroundPlane3D] CreateDepthStencilState failed: 0x" << std::hex << hr;
        Shutdown();
        return false;
    }

    LOG(INFO) << "[GroundPlane3D] Initialized";
    return true;
}

// ---------------------------------------------------------------------------
// Shutdown
// ---------------------------------------------------------------------------

void GroundPlane3D::Shutdown() {
    if (m_ds)      { m_ds->Release();      m_ds      = nullptr; }
    if (m_rast)    { m_rast->Release();    m_rast    = nullptr; }
    if (m_blend)   { m_blend->Release();   m_blend   = nullptr; }
    if (m_sampler) { m_sampler->Release(); m_sampler = nullptr; }
    if (m_cb)      { m_cb->Release();      m_cb      = nullptr; }
    if (m_vb)      { m_vb->Release();      m_vb      = nullptr; }
    if (m_layout)  { m_layout->Release();  m_layout  = nullptr; }
    if (m_ps)      { m_ps->Release();      m_ps      = nullptr; }
    if (m_vs)      { m_vs->Release();      m_vs      = nullptr; }
    m_device  = nullptr;
    m_context = nullptr;
}

// ---------------------------------------------------------------------------
// Render
// ---------------------------------------------------------------------------

void GroundPlane3D::Render(const Camera3D& camera, MinimapTileCache& minimapCache) {
    if (!m_vs || !m_device || !m_context) return;

    ID3D11DeviceContext* ctx = m_context;

    // Upload VP matrix to constant buffer
    {
        D3D11_MAPPED_SUBRESOURCE mapped = {};
        if (FAILED(ctx->Map(m_cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
            return;
        memcpy(mapped.pData, &camera.viewProj, sizeof(float) * 16);
        ctx->Unmap(m_cb, 0);
    }

    // Viewport (match camera viewport)
    D3D11_VIEWPORT dvp = {};
    dvp.TopLeftX = camera.vpX;
    dvp.TopLeftY = camera.vpY;
    dvp.Width    = camera.vpW;
    dvp.Height   = camera.vpH;
    dvp.MinDepth = 0.0f;
    dvp.MaxDepth = 1.0f;
    ctx->RSSetViewports(1, &dvp);

    // Set pipeline state
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->IASetInputLayout(m_layout);
    ctx->VSSetShader(m_vs, nullptr, 0);
    ctx->PSSetShader(m_ps, nullptr, 0);

    ctx->VSSetConstantBuffers(0, 1, &m_cb);

    ctx->PSSetSamplers(0, 1, &m_sampler);

    float bf[4] = {0, 0, 0, 0};
    ctx->OMSetBlendState(m_blend, bf, 0xFFFFFFFF);
    ctx->RSSetState(m_rast);
    ctx->OMSetDepthStencilState(m_ds, 0);

    UINT stride = sizeof(QuadVertex);
    UINT offset = 0;
    ctx->IASetVertexBuffers(0, 1, &m_vb, &stride, &offset);

    // Frustum planes for culling
    float frustum[6][4];
    camera.GetFrustumPlanes(frustum);

    // Draw each cached minimap tile as a textured quad at Z=0
    minimapCache.ForEachCachedTile([&](int tx, int ty,
                                       ID3D11ShaderResourceView* srv) {
        // Compute world-space bounds for this tile
        float xMin = (31 - tx) * kTileSize;
        float xMax = (32 - tx) * kTileSize;
        float yMin = (31 - ty) * kTileSize;
        float yMax = (32 - ty) * kTileSize;

        // Frustum cull: test flat AABB (Z=0 thin slab)
        bool visible = true;
        for (int i = 0; i < 6; ++i) {
            // P-vertex (most positive corner along plane normal)
            float px = (frustum[i][0] >= 0) ? xMax : xMin;
            float py = (frustum[i][1] >= 0) ? yMax : yMin;
            float pz = (frustum[i][2] >= 0) ? 0.0f : 0.0f; // Z is always 0
            float d = frustum[i][0] * px + frustum[i][1] * py +
                      frustum[i][2] * pz + frustum[i][3];
            if (d < 0) { visible = false; break; }
        }
        if (!visible) return;

        // Build 6 vertices (2 triangles) for the quad at Z=0
        //
        // UV mapping: WoW X+ = south, Y+ = west.
        // BLP image: row 0 = north (top), last row = south (bottom).
        // North-west corner = (xMin, yMax) -> UV (0, 0)
        // North-east corner = (xMin, yMin) -> UV (1, 0)
        // South-east corner = (xMax, yMin) -> UV (1, 1)
        // South-west corner = (xMax, yMax) -> UV (0, 1)
        QuadVertex verts[6] = {
            // Triangle 1: NW -> SE -> NE
            {xMin, yMax, 0.0f,  0.0f, 0.0f},  // NW
            {xMax, yMin, 0.0f,  1.0f, 1.0f},  // SE
            {xMin, yMin, 0.0f,  1.0f, 0.0f},  // NE

            // Triangle 2: NW -> SW -> SE
            {xMin, yMax, 0.0f,  0.0f, 0.0f},  // NW
            {xMax, yMax, 0.0f,  0.0f, 1.0f},  // SW
            {xMax, yMin, 0.0f,  1.0f, 1.0f},  // SE
        };

        // Upload quad vertices
        D3D11_MAPPED_SUBRESOURCE mapped = {};
        if (FAILED(ctx->Map(m_vb, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
            return;
        memcpy(mapped.pData, verts, sizeof(verts));
        ctx->Unmap(m_vb, 0);

        // Bind tile texture
        ctx->PSSetShaderResources(0, 1, &srv);

        // Draw
        ctx->Draw(6, 0);
    });

    // Unbind SRV to avoid warnings
    ID3D11ShaderResourceView* nullSRV = nullptr;
    ctx->PSSetShaderResources(0, 1, &nullSRV);
}

} // namespace mapedit
