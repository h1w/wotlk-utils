#include "navmesh_renderer.h"
#include "../canvas/canvas.h"
#include "../navmesh/tile_cache.h"
#include "../navmesh/tile_loader.h"

#include <imgui.h>
#include <glog/logging.h>

#include <algorithm>
#include <cstring>

namespace mapedit {

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static void BuildVertexData(const TileTriangles& tris, std::vector<NavmeshVertex>& out) {
    out.clear();
    out.reserve(tris.triangles.size() * 3);
    for (const auto& tri : tris.triangles) {
        out.push_back({tri.x[0], tri.y[0], 1.0f, 0.0f, 0.0f});
        out.push_back({tri.x[1], tri.y[1], 0.0f, 1.0f, 0.0f});
        out.push_back({tri.x[2], tri.y[2], 0.0f, 0.0f, 1.0f});
    }
}

static void ComputeAABB(const TileTriangles& tris,
                        float& minX, float& minY, float& maxX, float& maxY) {
    minX = minY =  1e30f;
    maxX = maxY = -1e30f;
    for (const auto& tri : tris.triangles) {
        for (int v = 0; v < 3; ++v) {
            if (tri.x[v] < minX) minX = tri.x[v];
            if (tri.x[v] > maxX) maxX = tri.x[v];
            if (tri.y[v] < minY) minY = tri.y[v];
            if (tri.y[v] > maxY) maxY = tri.y[v];
        }
    }
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

bool NavmeshRenderer::Initialize(ID3D11Device* device, ID3D11DeviceContext* context) {
    m_device  = device;
    m_context = context;
    if (!m_pipeline.Initialize(device)) {
        LOG(ERROR) << "[NavmeshRenderer] Pipeline init failed";
        return false;
    }
    LOG(INFO) << "[NavmeshRenderer] GPU pipeline ready";
    return true;
}

void NavmeshRenderer::Shutdown() {
    for (auto& [key, tile] : m_gpuCache)
        ReleaseGpuTile(tile);
    m_gpuCache.clear();
    m_pipeline.Shutdown();
    m_device  = nullptr;
    m_context = nullptr;
}

// ---------------------------------------------------------------------------
// GPU cache management
// ---------------------------------------------------------------------------

void NavmeshRenderer::ReleaseGpuTile(TileGpuData& t) {
    if (t.detailVB) { t.detailVB->Release(); t.detailVB = nullptr; }
    if (t.baseVB)   { t.baseVB->Release();   t.baseVB = nullptr; }
}

ID3D11Buffer* NavmeshRenderer::CreateVB(const std::vector<NavmeshVertex>& verts) {
    if (verts.empty() || !m_device) return nullptr;

    D3D11_BUFFER_DESC desc = {};
    desc.ByteWidth = static_cast<UINT>(verts.size() * sizeof(NavmeshVertex));
    desc.Usage     = D3D11_USAGE_IMMUTABLE;
    desc.BindFlags = D3D11_BIND_VERTEX_BUFFER;

    D3D11_SUBRESOURCE_DATA init = {};
    init.pSysMem = verts.data();

    ID3D11Buffer* buf = nullptr;
    if (FAILED(m_device->CreateBuffer(&desc, &init, &buf)))
        return nullptr;
    return buf;
}

void NavmeshRenderer::SyncGpuCache(TileCache& cache) {
    uint32_t mapId = cache.GetMapId();

    // Map changed — flush everything
    if (mapId != m_lastSyncMapId) {
        for (auto& [k, t] : m_gpuCache) ReleaseGpuTile(t);
        m_gpuCache.clear();
        m_lastSyncMapId = mapId;
    }

    // Collect keys of tiles currently in TileCache
    std::map<TileKey, bool> loaded;
    cache.ForEachTile([&](int tx, int ty, const TileTriangles&, int, int) {
        loaded[{tx, ty}] = true;
    });

    // Evict GPU entries no longer in TileCache
    for (auto it = m_gpuCache.begin(); it != m_gpuCache.end(); ) {
        if (loaded.count(it->first) == 0) {
            ReleaseGpuTile(it->second);
            it = m_gpuCache.erase(it);
        } else {
            ++it;
        }
    }

    // Upload new tiles (throttled: max 8 new VBs per frame)
    static constexpr int kMaxGpuUploadsPerFrame = 8;
    int gpuUploads = 0;
    cache.ForEachTile([&](int tx, int ty, const TileTriangles& tris,
                          int detourX, int detourY) {
        TileKey key = {tx, ty};
        if (m_gpuCache.count(key)) return;
        if (gpuUploads >= kMaxGpuUploadsPerFrame) return;

        TileGpuData gpu;

        // Detail VB
        std::vector<NavmeshVertex> verts;
        BuildVertexData(tris, verts);
        gpu.detailVB        = CreateVB(verts);
        gpu.detailVertCount = static_cast<UINT>(verts.size());

        // Base polygon VB (LOD) — use exact Detour coords
        if (cache.GetNavMesh()) {
            TileTriangles base = ExtractBasePolygons(cache.GetNavMesh(), tx, ty,
                                                      detourX, detourY);
            std::vector<NavmeshVertex> bv;
            BuildVertexData(base, bv);
            gpu.baseVB        = CreateVB(bv);
            gpu.baseVertCount = static_cast<UINT>(bv.size());
        }

        // AABB from detail triangles
        ComputeAABB(tris, gpu.minX, gpu.minY, gpu.maxX, gpu.maxY);

        m_gpuCache[key] = gpu;
        ++gpuUploads;
    });
}

// ---------------------------------------------------------------------------
// Render (called each frame from App::RenderFrame)
// ---------------------------------------------------------------------------

void NavmeshRenderer::Render(const Canvas& canvas, TileCache& cache, float minZoom) {
    if (canvas.zoom < minZoom) return;
    if (!m_pipeline.IsReady() || !m_device) return;

    SyncGpuCache(cache);

    bool drawEdges = (canvas.zoom >= 0.1f);

    m_cbData.tiles.clear();

    // Use static VBs from GPU cache.
    // TileCache already loads only viewport-relevant tiles,
    // and SyncGpuCache keeps the GPU cache in sync.
    if (m_gpuCache.empty()) return;

    bool useDetail = (canvas.zoom >= 0.1f);
    for (const auto& [key, tile] : m_gpuCache) {
        ID3D11Buffer* vb = useDetail ? tile.detailVB : tile.baseVB;
        UINT vc          = useDetail ? tile.detailVertCount : tile.baseVertCount;

        // Fallback to detail if base unavailable
        if (!vb || vc == 0) { vb = tile.detailVB; vc = tile.detailVertCount; }
        if (!vb || vc == 0) continue;

        m_cbData.tiles.push_back({vb, vc});
    }

    // Log first tile's AABB to verify vertex coordinate range
    if (!m_gpuCache.empty()) {
        auto it = m_gpuCache.begin();
    }

    if (m_cbData.tiles.empty()) return;

    // Populate callback data
    m_cbData.self      = this;
    m_cbData.context   = m_context;
    m_cbData.vpX       = canvas.vpX;
    m_cbData.vpY       = canvas.vpY;
    m_cbData.vpW       = canvas.vpW;
    m_cbData.vpH       = canvas.vpH;
    m_cbData.centerX   = canvas.centerX;
    m_cbData.centerY   = canvas.centerY;
    m_cbData.zoom      = canvas.zoom;
    m_cbData.drawEdges = drawEdges;

    // Inject DX11 draw calls into ImGui's background draw list
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    dl->AddCallback(DrawCallback, &m_cbData);
    dl->AddCallback(ImDrawCallback_ResetRenderState, nullptr);
}

// ---------------------------------------------------------------------------
// DX11 draw callback (called by ImGui_ImplDX11_RenderDrawData)
// ---------------------------------------------------------------------------

void NavmeshRenderer::DrawCallback(const ImDrawList* /*parent_list*/,
                                   const ImDrawCmd* cmd) {
    auto* data = static_cast<CallbackData*>(cmd->UserCallbackData);
    if (!data || !data->self || !data->context) return;

    NavmeshPipeline& p   = data->self->m_pipeline;
    ID3D11DeviceContext* ctx = data->context;

    // Viewport
    D3D11_VIEWPORT vp = {};
    vp.TopLeftX = data->vpX;
    vp.TopLeftY = data->vpY;
    vp.Width    = data->vpW;
    vp.Height   = data->vpH;
    vp.MinDepth = 0.0f;
    vp.MaxDepth = 1.0f;
    ctx->RSSetViewports(1, &vp);

    // Scissor
    D3D11_RECT sr = {
        static_cast<LONG>(data->vpX),
        static_cast<LONG>(data->vpY),
        static_cast<LONG>(data->vpX + data->vpW),
        static_cast<LONG>(data->vpY + data->vpH)
    };
    ctx->RSSetScissorRects(1, &sr);

    // Pipeline state
    ctx->VSSetShader(p.GetVS(), nullptr, 0);
    ctx->PSSetShader(p.GetPS(), nullptr, 0);
    ctx->IASetInputLayout(p.GetInputLayout());
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    float bf[4] = {0, 0, 0, 0};
    ctx->OMSetBlendState(p.GetBlendState(), bf, 0xFFFFFFFF);
    ctx->RSSetState(p.GetRastState());
    ctx->OMSetDepthStencilState(p.GetDSState(), 0);

    // Constant buffer
    NavmeshCB cb = {};
    cb.viewCenterX = data->centerX;
    cb.viewCenterY = data->centerY;
    cb.scaleX      = 2.0f * data->zoom / data->vpW;
    cb.scaleY      = 2.0f * data->zoom / data->vpH;

    cb.fillR = 0.0f;            cb.fillG = 180.0f / 255.0f;
    cb.fillB = 80.0f / 255.0f;  cb.fillA = 40.0f / 255.0f;

    if (data->drawEdges) {
        cb.edgeR = 0.0f;            cb.edgeG = 200.0f / 255.0f;
        cb.edgeB = 100.0f / 255.0f; cb.edgeA = 100.0f / 255.0f;
    } else {
        cb.edgeR = cb.fillR; cb.edgeG = cb.fillG;
        cb.edgeB = cb.fillB; cb.edgeA = cb.fillA;
    }

    D3D11_MAPPED_SUBRESOURCE mapped = {};
    if (SUCCEEDED(ctx->Map(p.GetConstantBuffer(), 0,
                           D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
        memcpy(mapped.pData, &cb, sizeof(cb));
        ctx->Unmap(p.GetConstantBuffer(), 0);
    }

    ID3D11Buffer* cbBuf = p.GetConstantBuffer();
    ctx->VSSetConstantBuffers(0, 1, &cbBuf);
    ctx->PSSetConstantBuffers(0, 1, &cbBuf);

    // Draw visible tiles
    UINT stride = sizeof(NavmeshVertex);
    UINT offset = 0;
    for (const auto& t : data->tiles) {
        ctx->IASetVertexBuffers(0, 1, &t.vb, &stride, &offset);
        ctx->Draw(t.vertCount, 0);
    }
}

} // namespace mapedit
