# Replacing 800K ImGui triangles with native DX11 rendering

**The single highest-impact change is moving all navmesh geometry into static DX11 vertex/index buffers and rendering them with a custom shader pipeline, eliminating ImGui's per-frame CPU vertex generation entirely.** This alone transforms ~100–200 ms of CPU draw-list construction into a sub-millisecond operation — a **100×+ speedup** on the rendering bottleneck. Combined with CPU viewport culling and a tile-as-texture LOD system, the editor can handle 800K+ triangles at interactive framerates regardless of zoom level. Below is a complete implementation guide with production-ready C++ and HLSL code.

---

## Why ImGui is the wrong tool for 800K triangles

ImGui's `AddTriangleFilled` is designed for UI widgets — hundreds to low thousands of primitives. Each call performs vertex allocation, color packing, UV assignment, bounds checking, and (with the default `AntiAliasedFill = true`) generates **3× the geometry** for anti-aliased edge fringes. For 800,000 triangles:

- **With AA on**: ~7.2M vertices, ~16.8M indices, ~140 MB of vertex data rebuilt every frame on the CPU
- **With AA off**: ~2.4M vertices, ~2.4M indices, ~46 MB per frame
- **CPU time**: **50–200+ ms per frame** just constructing the draw list, before any GPU work

A single DX11 `DrawIndexed` call with a static vertex buffer costs **< 0.01 ms** of CPU time. The GPU renders 800K simple 2D triangles in well under 1 ms on any modern discrete or integrated GPU. The entire rendering path shifts from CPU-bound to essentially free.

---

## The recommended architecture: render-to-texture with AddCallback fallback

Three viable integration patterns exist for custom DX11 rendering alongside ImGui, ranked by overall effectiveness:

**Render before ImGui (best for this case).** Render the navmesh to the backbuffer (or an offscreen texture) before calling `ImGui::Render()`. ImGui's UI then composites on top. This is the cleanest separation — no state management conflicts, no callbacks. For a fullscreen map editor where the navmesh fills the background, this is ideal.

**`ImDrawList::AddCallback()` on the background draw list.** Injects a function pointer into ImGui's command stream. During `ImGui_ImplDX11_RenderDrawData`, the backend calls your function instead of issuing a draw. You switch to your custom pipeline, draw, then insert `ImDrawCallback_ResetRenderState` to restore ImGui's state. This works well when the navmesh must render inside a specific ImGui window.

**Render-to-texture displayed via `ImGui::Image()`.** Render the navmesh to an `ID3D11Texture2D` with both `BIND_RENDER_TARGET` and `BIND_SHADER_RESOURCE`, then display the `ID3D11ShaderResourceView*` as an `ImTextureID`. This enables **frame caching** — skip re-rendering when neither the camera nor navmesh data has changed. Best for the LOD system described later.

For a map editor with pan/zoom, **render before ImGui** is the primary path at normal zoom, with **render-to-texture caching** activated at low zoom for LOD. Here is the complete callback approach for rendering inside an ImGui window:

```cpp
// Callback injected into ImGui's background draw list
void NavmeshDrawCallback(const ImDrawList* parent_list, const ImDrawCmd* cmd) {
    auto* rs = (ImGui_ImplDX11_RenderState*)ImGui::GetPlatformIO().Renderer_RenderState;
    ID3D11DeviceContext* ctx = rs->DeviceContext;
    auto* data = (NavmeshRenderData*)cmd->UserCallbackData;

    UINT stride = sizeof(NavmeshVertex), offset = 0;
    ctx->IASetInputLayout(data->inputLayout);
    ctx->IASetVertexBuffers(0, 1, &data->vertexBuffer, &stride, &offset);
    ctx->IASetIndexBuffer(data->indexBuffer, DXGI_FORMAT_R32_UINT, 0);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->VSSetShader(data->vertexShader, nullptr, 0);
    ctx->VSSetConstantBuffers(0, 1, &data->constantBuffer);
    ctx->PSSetShader(data->pixelShader, nullptr, 0);
    ctx->OMSetBlendState(data->blendState, nullptr, 0xFFFFFFFF);
    ctx->OMSetDepthStencilState(data->depthStencilState, 0);
    ctx->RSSetState(data->rasterizerState);
    ctx->RSSetViewports(1, &data->viewport);
    ctx->DrawIndexed(data->indexCount, 0, 0);
}

// In frame loop, between ImGui::NewFrame() and ImGui::Render():
ImDrawList* bg = ImGui::GetBackgroundDrawList();
bg->AddCallback(NavmeshDrawCallback, &g_navmeshData);
bg->AddCallback(ImDrawCallback_ResetRenderState, nullptr);
```

---

## Complete HLSL shaders for 2D navmesh with zoom and pan

The vertex shader applies the canvas transform — `screenX = vpCenterX + (worldY - centerY) × zoom` — entirely on the GPU. World-space vertex positions are uploaded **once** and never change. Only the **32-byte constant buffer** updates each frame when the camera moves.

```hlsl
// NavmeshVS.hlsl
cbuffer TransformCB : register(b0) {
    float2 viewCenter;   // world-space camera center (centerX, centerY)
    float2 vpCenter;     // viewport center in pixels (vpCenterX, vpCenterY)
    float  zoom;         // pixels per world unit
    float  vpWidth;      // viewport width in pixels
    float  vpHeight;     // viewport height in pixels
    float  _pad;
};

struct VS_IN  { float2 pos : POSITION; float4 color : COLOR; };
struct VS_OUT { float4 pos : SV_POSITION; float4 color : COLOR; };

VS_OUT main(VS_IN i) {
    VS_OUT o;
    // World → screen pixels (note: X/Y axes swapped per your transform)
    float sx = vpCenter.x + (i.pos.y - viewCenter.y) * zoom;
    float sy = vpCenter.y - (i.pos.x - viewCenter.x) * zoom;
    // Screen pixels → clip-space NDC
    o.pos = float4(sx / vpWidth * 2.0 - 1.0,
                   1.0 - sy / vpHeight * 2.0, 0.0, 1.0);
    o.color = i.color;
    return o;
}
```

```hlsl
// NavmeshPS.hlsl — flat color with alpha, blending via blend state
struct PS_IN { float4 pos : SV_POSITION; float4 color : COLOR; };
float4 main(PS_IN i) : SV_TARGET { return i.color; }
```

For **single-pass filled triangles with wireframe overlay**, use barycentric coordinates passed as a vertex attribute. This avoids a geometry shader and delivers anti-aliased, pixel-consistent 1 px edges:

```hlsl
// NavmeshWirePS.hlsl — fill + wireframe in a single pass
struct PS_IN {
    float4 pos   : SV_POSITION;
    float4 color : COLOR;
    float3 bary  : TEXCOORD0;   // (1,0,0), (0,1,0), (0,0,1) per triangle corner
};

float4 main(PS_IN i) : SV_TARGET {
    float3 dx = ddx(i.bary);
    float3 dy = ddy(i.bary);
    float3 rate = sqrt(dx * dx + dy * dy);  // screen-space change rate

    float thickness = 1.0;  // edge width in pixels
    float falloff   = 1.0;  // anti-aliasing transition
    float3 remap = smoothstep(rate * thickness, rate * (thickness + falloff), i.bary);
    float wire = min(remap.x, min(remap.y, remap.z));

    float4 edgeColor = float4(0, 0, 0, 1);
    return lerp(edgeColor, i.color, wire);
}
```

The `ddx`/`ddy` trick maps pixel units to barycentric units, producing **perfectly consistent 1 px lines regardless of zoom level or triangle size**. The `smoothstep` provides anti-aliasing. This single-pass approach eliminates the need for a second wireframe draw call.

---

## Complete DX11 pipeline setup in C++

```cpp
struct NavmeshVertex { float x, y; uint32_t color; };  // 12 bytes
// For wireframe: { float x, y; uint32_t color; float baryX, baryY, baryZ; } = 24 bytes

struct NavmeshCB {  // 32 bytes, 16-byte aligned
    float viewCenterX, viewCenterY;
    float vpCenterX, vpCenterY;
    float zoom, vpWidth, vpHeight, _pad;
};

bool CreateNavmeshPipeline(ID3D11Device* dev, NavmeshPipeline& p) {
    // Compile shaders (embedded HLSL or from file)
    ID3DBlob *vsBlob = nullptr, *psBlob = nullptr, *err = nullptr;
    D3DCompile(vsSource, strlen(vsSource), nullptr, nullptr, nullptr,
               "main", "vs_5_0", 0, 0, &vsBlob, &err);
    D3DCompile(psSource, strlen(psSource), nullptr, nullptr, nullptr,
               "main", "ps_5_0", 0, 0, &psBlob, &err);
    dev->CreateVertexShader(vsBlob->GetBufferPointer(),
                            vsBlob->GetBufferSize(), nullptr, &p.vs);
    dev->CreatePixelShader(psBlob->GetBufferPointer(),
                           psBlob->GetBufferSize(), nullptr, &p.ps);

    // Input layout: POSITION (R32G32_FLOAT) + COLOR (R8G8B8A8_UNORM)
    D3D11_INPUT_ELEMENT_DESC layout[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT,  0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"COLOR",    0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 8, D3D11_INPUT_PER_VERTEX_DATA, 0},
    };
    dev->CreateInputLayout(layout, 2, vsBlob->GetBufferPointer(),
                           vsBlob->GetBufferSize(), &p.inputLayout);

    // Blend state: standard alpha blending
    D3D11_BLEND_DESC bd = {};
    bd.RenderTarget[0].BlendEnable    = TRUE;
    bd.RenderTarget[0].SrcBlend       = D3D11_BLEND_SRC_ALPHA;
    bd.RenderTarget[0].DestBlend      = D3D11_BLEND_INV_SRC_ALPHA;
    bd.RenderTarget[0].BlendOp        = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].SrcBlendAlpha  = D3D11_BLEND_ONE;
    bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    bd.RenderTarget[0].BlendOpAlpha   = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    dev->CreateBlendState(&bd, &p.blendState);

    // Rasterizer: 2D, no culling, scissor enabled
    D3D11_RASTERIZER_DESC rd = {};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.ScissorEnable = TRUE;
    rd.DepthClipEnable = TRUE;
    dev->CreateRasterizerState(&rd, &p.rasterizerState);

    // Depth-stencil: completely disabled
    D3D11_DEPTH_STENCIL_DESC dd = {};
    dd.DepthEnable = FALSE;  dd.StencilEnable = FALSE;
    dev->CreateDepthStencilState(&dd, &p.depthStencilState);

    // Dynamic constant buffer for camera transform
    D3D11_BUFFER_DESC cbd = {};
    cbd.ByteWidth = sizeof(NavmeshCB);
    cbd.Usage = D3D11_USAGE_DYNAMIC;
    cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    dev->CreateBuffer(&cbd, nullptr, &p.cb);
    return true;
}
```

---

## Buffer strategy: per-tile IMMUTABLE with batched draws

Four buffer strategies apply to this problem. The right choice depends on whether tiles load/unload dynamically and how many draw calls the application can tolerate.

**Per-tile IMMUTABLE buffers** are the simplest and recommended starting point. Create one `D3D11_USAGE_IMMUTABLE` vertex buffer and one index buffer per tile at load time. Release them on tile eviction. Each visible tile is one `DrawIndexed` call. With 50–100 visible tiles, that's 50–100 draw calls — well within DX11's comfort zone (**~5,000 draw calls per frame** is a typical budget). Memory: 200 tiles × 4,000 triangles × 3 vertices × 12 bytes = **~28.8 MB** for vertices plus **~9.6 MB** for 32-bit indices. Under 40 MB total — trivial for any GPU.

```cpp
void CreateTileBuffers(ID3D11Device* dev, Tile& tile,
                       const NavmeshVertex* verts, UINT vertCount,
                       const uint32_t* indices, UINT idxCount) {
    D3D11_BUFFER_DESC vbd = {};
    vbd.ByteWidth = vertCount * sizeof(NavmeshVertex);
    vbd.Usage = D3D11_USAGE_IMMUTABLE;
    vbd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    D3D11_SUBRESOURCE_DATA vsd = { verts };
    dev->CreateBuffer(&vbd, &vsd, &tile.vb);

    D3D11_BUFFER_DESC ibd = {};
    ibd.ByteWidth = idxCount * sizeof(uint32_t);
    ibd.Usage = D3D11_USAGE_IMMUTABLE;
    ibd.BindFlags = D3D11_BIND_INDEX_BUFFER;
    D3D11_SUBRESOURCE_DATA isd = { indices };
    dev->CreateBuffer(&ibd, &isd, &tile.ib);
    tile.indexCount = idxCount;
}
```

**Large batched DYNAMIC buffer** merges all visible tile geometry into one buffer each frame, enabling a single `DrawIndexed` call. CPU cost: copying ~200–400K vertices (~2.4–4.8 MB) via `Map`/`Unmap` per frame. This is ~100× less data than ImGui generates. Use `D3D11_MAP_WRITE_DISCARD` for simple frame-by-frame rebuilding:

```cpp
// Pre-allocate large dynamic buffers (worst-case capacity)
D3D11_BUFFER_DESC vbd = {};
vbd.ByteWidth = MAX_VISIBLE_TILES * MAX_VERTS_PER_TILE * sizeof(NavmeshVertex);
vbd.Usage = D3D11_USAGE_DYNAMIC;
vbd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
vbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
dev->CreateBuffer(&vbd, nullptr, &g_batchVB);

// Each frame: fill with visible tile data
D3D11_MAPPED_SUBRESOURCE mapped;
ctx->Map(g_batchVB, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
auto* dst = (NavmeshVertex*)mapped.pData;
UINT totalVerts = 0, totalIndices = 0;
for (auto* tile : visibleTiles) {
    memcpy(dst + totalVerts, tile->vertices.data(),
           tile->vertices.size() * sizeof(NavmeshVertex));
    // Offset indices by totalVerts for this tile's chunk
    totalVerts += (UINT)tile->vertices.size();
}
ctx->Unmap(g_batchVB, 0);
ctx->DrawIndexed(totalIndices, 0, 0);  // One call for everything
```

**Recommendation**: Start with per-tile IMMUTABLE buffers. If profiling shows draw call overhead matters (unlikely at 50–100 calls), switch to the batched approach. The IMMUTABLE path requires zero per-frame CPU work for geometry — only the 32-byte constant buffer update. **This is the biggest win: zero CPU vertex work vs. 100+ ms of ImGui vertex generation.**

For the **barycentric wireframe** variant, vertices expand from 12 to 24 bytes (adding `float3 bary`), and shared vertices must be duplicated so each triangle gets unique (1,0,0), (0,1,0), (0,0,1) attributes. Memory rises to ~57.6 MB for vertices — still well within budget. Build these expanded vertices at tile load time:

```cpp
struct WireVertex { float x, y; uint32_t color; float baryX, baryY, baryZ; };

void BuildWireframeVertices(const NavmeshVertex* src, const uint32_t* idx,
                            UINT triCount, std::vector<WireVertex>& out) {
    out.reserve(triCount * 3);
    for (UINT i = 0; i < triCount; i++) {
        auto& v0 = src[idx[i*3+0]], &v1 = src[idx[i*3+1]], &v2 = src[idx[i*3+2]];
        out.push_back({v0.x, v0.y, v0.color, 1,0,0});
        out.push_back({v1.x, v1.y, v1.color, 0,1,0});
        out.push_back({v2.x, v2.y, v2.color, 0,0,1});
    }
    // No index buffer needed — draw with Draw(vertCount, 0)
}
```

---

## CPU viewport culling eliminates 50–75% of work for free

Each tile has a known AABB (533.33 world units per side). The visible world rectangle is trivially derived by inverting the canvas transform. Testing 200 AABBs costs **~2 µs** and typically eliminates half or more of all tiles when zoomed in.

```cpp
struct WorldRect { float minX, minY, maxX, maxY; };

WorldRect ComputeVisibleWorldRect(float centerX, float centerY, float zoom,
                                  float screenW, float screenH) {
    float halfW = (screenW * 0.5f) / zoom;
    float halfH = (screenH * 0.5f) / zoom;
    // Account for the X↔Y axis swap in the canvas transform
    return { centerX - halfH, centerY - halfW,
             centerX + halfH, centerY + halfW };
}

bool TileIsVisible(const TileAABB& tile, const WorldRect& view) {
    return tile.maxX >= view.minX && tile.minX <= view.maxX
        && tile.maxY >= view.minY && tile.minY <= view.maxY;
}
```

Add a screen-area threshold to skip tiles too small to contribute pixels:

```cpp
float screenArea = (tile.sizeX * zoom) * (tile.sizeY * zoom);
if (screenArea < 16.0f) continue;  // sub-4px tile, invisible
```

GPU-side clipping (letting the vertex shader project off-screen triangles that get clipped) still processes every vertex of every submitted tile. **CPU culling avoids both the draw call overhead and the vertex shader invocations entirely.** For 200 tiles, CPU culling is strictly better and essentially free.

---

## Tile-as-texture LOD for extreme zoom levels

At low zoom, a 533-unit tile might occupy fewer pixels than it has triangles. Rendering 4,000 triangles to produce a 20×20 pixel region is wasteful. The solution: **pre-render each tile to a 128×128 or 256×256 texture once**, then draw a single textured quad per tile.

```cpp
struct TileCache {
    ID3D11Texture2D*          texture = nullptr;
    ID3D11RenderTargetView*   rtv = nullptr;
    ID3D11ShaderResourceView* srv = nullptr;
    bool dirty = true;
};

void CreateTileCache(ID3D11Device* dev, TileCache& c, UINT size = 256) {
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = size;  td.Height = size;  td.MipLevels = 1;  td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    dev->CreateTexture2D(&td, nullptr, &c.texture);
    dev->CreateRenderTargetView(c.texture, nullptr, &c.rtv);
    dev->CreateShaderResourceView(c.texture, nullptr, &c.srv);
}
```

Display via ImGui's background draw list:

```cpp
ImVec2 screenMin = WorldToScreen(tile.aabb.minX, tile.aabb.minY);
ImVec2 screenMax = WorldToScreen(tile.aabb.maxX, tile.aabb.maxY);
ImGui::GetBackgroundDrawList()->AddImage((ImTextureID)cache.srv, screenMin, screenMax);
```

Memory at 256×256 RGBA8: **256 KB per tile × 200 tiles = 50 MB**. At 128×128: **12.5 MB total**. Invalidate and re-render only when tile data changes. This converts 800K triangles into **~200 textured quads** at low zoom — a **4,000× triangle reduction**.

---

## Wireframe edges: barycentric attributes beat all alternatives

Five approaches exist for overlaying wireframe edges on filled navmesh triangles. The barycentric vertex attribute method dominates for this use case:

- **DX11 `D3D11_FILL_WIREFRAME` rasterizer state** is the simplest (two-pass: fill then wireframe) but produces aliased lines, renders internal edges, and causes z-fighting requiring depth bias. Not recommended.
- **Geometry shader** (NVIDIA's "Solid Wireframe" paper) computes edge distance in screen-space. Excellent quality but geometry shaders add pipeline latency and reduce throughput by **~30%** on typical hardware.
- **Separate line primitives** with `D3D11_PRIMITIVE_TOPOLOGY_LINELIST` require a second draw call and a deduplicated edge index buffer. Acceptable but aliased without MSAA.
- **Barycentric vertex attributes with `ddx`/`ddy`** (recommended) require no geometry shader, render in a single pass, and produce anti-aliased pixel-consistent edges. The cost is 2× vertex memory (24 vs 12 bytes, since vertices cannot be shared). At ~58 MB total for 200 tiles, this is very affordable.
- **`SV_Barycentrics`** requires Shader Model 6.1+ (DX12) and is unavailable on portable DX11.

The barycentric approach's `ddx`/`ddy` derivative trick automatically adjusts edge width to remain exactly 1 pixel regardless of zoom level or triangle size. The `smoothstep` transition provides sub-pixel anti-aliasing. This is the clear winner for a DX11 map editor.

---

## Putting it all together: the rendering decision tree

```cpp
enum class RenderMode { Skip, TexturedQuad, FullGeometry };

RenderMode ChooseMode(const Tile& tile, const WorldRect& view, float zoom) {
    if (!TileIsVisible(tile.aabb, view)) return RenderMode::Skip;
    float screenSize = tile.worldSize * zoom;
    if (screenSize < 4.0f) return RenderMode::Skip;
    if (screenSize < 64.0f) return RenderMode::TexturedQuad;  // sub-64px, use cache
    return RenderMode::FullGeometry;
}

void RenderFrame(ID3D11DeviceContext* ctx, float zoom, /*...*/) {
    // Update constant buffer (32 bytes, once per frame)
    UpdateTransformCB(ctx, pipeline.cb, centerX, centerY,
                      vpCenterX, vpCenterY, zoom, vpW, vpH);

    WorldRect view = ComputeVisibleWorldRect(centerX, centerY, zoom, screenW, screenH);

    for (auto& tile : allTiles) {
        switch (ChooseMode(tile, view, zoom)) {
        case RenderMode::Skip: break;
        case RenderMode::TexturedQuad:
            if (tile.cache.dirty) RenderTileToTexture(ctx, tile);
            DrawCachedQuad(tile); break;
        case RenderMode::FullGeometry:
            BindTileBuffers(ctx, tile);
            ctx->Draw(tile.vertexCount, 0); break;  // barycentric, no IB
        }
    }
}
```

**Expected performance at each zoom level:**

| Scenario | Visible tiles | Triangles drawn | Draw calls | CPU time |
|----------|--------------|----------------|------------|----------|
| Current (ImGui, any zoom) | 200 | 800,000 | ~1 (batched) | **100–200 ms** |
| Close zoom (culled, full geo) | ~50 | ~200,000 | ~50 | **< 0.5 ms** |
| Medium zoom (culled, full geo) | ~100 | ~400,000 | ~100 | **< 1 ms** |
| Far zoom (culled + RTT LOD) | ~200 | ~400 (quads) | ~200 | **< 0.2 ms** |

---

## Conclusion

The path from 800K `AddTriangleFilled` calls to smooth real-time rendering requires exactly three changes, listed in priority order. **First**, move all triangle geometry into per-tile IMMUTABLE DX11 vertex buffers with the barycentric wireframe vertex format, built once at tile load. This eliminates the CPU bottleneck entirely and delivers filled + wireframe rendering in a single pass. **Second**, add CPU viewport culling — a 10-line AABB test that costs 2 µs and saves 50–75% of draw calls when zoomed in. **Third**, implement tile-as-texture caching for low zoom levels, converting 800K triangles into ~200 textured quads when individual triangles would be sub-pixel anyway.

Skip instanced rendering (tiles have unique geometry, so instancing doesn't apply), skip mesh simplification (the RTT approach is simpler and more effective), and skip the large batched dynamic buffer (per-tile static buffers at 50–100 draw calls are fine for DX11). The constant buffer approach — uploading world-space vertices once and changing only the 32-byte camera transform per frame — is the core architectural insight that makes everything else possible.