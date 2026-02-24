#include "navmesh_renderer_3d.h"
#include "../camera/camera3d.h"
#include "../navmesh/tile_cache.h"
#include "../navmesh/tile_loader.h"

#include <glog/logging.h>

#include <algorithm>
#include <cstring>
#include <cmath>

namespace mapedit {

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static void BuildVertexData3D(const TileTriangles& tris,
                               std::vector<NavmeshVertex3D>& out) {
    out.clear();
    out.reserve(tris.triangles.size() * 3);
    for (const auto& tri : tris.triangles) {
        out.push_back({tri.x[0], tri.y[0], tri.z[0], 1.0f, 0.0f, 0.0f});
        out.push_back({tri.x[1], tri.y[1], tri.z[1], 0.0f, 1.0f, 0.0f});
        out.push_back({tri.x[2], tri.y[2], tri.z[2], 0.0f, 0.0f, 1.0f});
    }
}

static void ComputeAABB3D(const TileTriangles& tris,
                           float& minX, float& minY, float& minZ,
                           float& maxX, float& maxY, float& maxZ) {
    minX = minY = minZ =  1e30f;
    maxX = maxY = maxZ = -1e30f;
    for (const auto& tri : tris.triangles) {
        for (int v = 0; v < 3; ++v) {
            if (tri.x[v] < minX) minX = tri.x[v];
            if (tri.x[v] > maxX) maxX = tri.x[v];
            if (tri.y[v] < minY) minY = tri.y[v];
            if (tri.y[v] > maxY) maxY = tri.y[v];
            if (tri.z[v] < minZ) minZ = tri.z[v];
            if (tri.z[v] > maxZ) maxZ = tri.z[v];
        }
    }
}

// ---------------------------------------------------------------------------
// Frustum culling
// ---------------------------------------------------------------------------

bool NavmeshRenderer3D::FrustumIntersectsAABB(const float planes[6][4],
                                                const TileGpuData3D& tile) {
    for (int i = 0; i < 6; ++i) {
        float px = (planes[i][0] >= 0) ? tile.maxX : tile.minX;
        float py = (planes[i][1] >= 0) ? tile.maxY : tile.minY;
        float pz = (planes[i][2] >= 0) ? tile.maxZ : tile.minZ;
        float d = planes[i][0] * px + planes[i][1] * py +
                  planes[i][2] * pz + planes[i][3];
        if (d < 0) return false; // entirely outside this plane
    }
    return true;
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

bool NavmeshRenderer3D::Initialize(ID3D11Device* device, ID3D11DeviceContext* context) {
    m_device  = device;
    m_context = context;
    if (!m_pipeline.Initialize(device)) {
        LOG(ERROR) << "[NavmeshRenderer3D] Pipeline init failed";
        return false;
    }
    LOG(INFO) << "[NavmeshRenderer3D] GPU pipeline ready";
    return true;
}

void NavmeshRenderer3D::Shutdown() {
    for (auto& [key, tile] : m_gpuCache)
        ReleaseGpuTile(tile);
    m_gpuCache.clear();
    m_pipeline.Shutdown();
    m_device  = nullptr;
    m_context = nullptr;
}

// ---------------------------------------------------------------------------
// GPU cache management (mirrors 2D NavmeshRenderer)
// ---------------------------------------------------------------------------

void NavmeshRenderer3D::ReleaseGpuTile(TileGpuData3D& t) {
    if (t.detailVB) { t.detailVB->Release(); t.detailVB = nullptr; }
    if (t.baseVB)   { t.baseVB->Release();   t.baseVB = nullptr; }
}

ID3D11Buffer* NavmeshRenderer3D::CreateVB(const std::vector<NavmeshVertex3D>& verts) {
    if (verts.empty() || !m_device) return nullptr;

    D3D11_BUFFER_DESC desc = {};
    desc.ByteWidth = static_cast<UINT>(verts.size() * sizeof(NavmeshVertex3D));
    desc.Usage     = D3D11_USAGE_IMMUTABLE;
    desc.BindFlags = D3D11_BIND_VERTEX_BUFFER;

    D3D11_SUBRESOURCE_DATA init = {};
    init.pSysMem = verts.data();

    ID3D11Buffer* buf = nullptr;
    if (FAILED(m_device->CreateBuffer(&desc, &init, &buf)))
        return nullptr;
    return buf;
}

void NavmeshRenderer3D::SyncGpuCache(TileCache& cache) {
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

    // Upload new tiles (throttled: max 8 per frame)
    static constexpr int kMaxGpuUploadsPerFrame = 8;
    int gpuUploads = 0;
    cache.ForEachTile([&](int tx, int ty, const TileTriangles& tris,
                          int detourX, int detourY) {
        TileKey key = {tx, ty};
        if (m_gpuCache.count(key)) return;
        if (gpuUploads >= kMaxGpuUploadsPerFrame) return;

        TileGpuData3D gpu;

        // Detail VB (with Z!)
        std::vector<NavmeshVertex3D> verts;
        BuildVertexData3D(tris, verts);
        gpu.detailVB        = CreateVB(verts);
        gpu.detailVertCount = static_cast<UINT>(verts.size());

        // Base polygon VB (LOD)
        if (cache.GetNavMesh()) {
            TileTriangles base = ExtractBasePolygons(cache.GetNavMesh(), tx, ty,
                                                      detourX, detourY);
            std::vector<NavmeshVertex3D> bv;
            BuildVertexData3D(base, bv);
            gpu.baseVB        = CreateVB(bv);
            gpu.baseVertCount = static_cast<UINT>(bv.size());
        }

        // 3D AABB
        ComputeAABB3D(tris, gpu.minX, gpu.minY, gpu.minZ,
                             gpu.maxX, gpu.maxY, gpu.maxZ);

        m_gpuCache[key] = gpu;
        ++gpuUploads;
    });
}

// ---------------------------------------------------------------------------
// Render (called directly from App::RenderFrame3D, NOT via ImGui callback)
// ---------------------------------------------------------------------------

void NavmeshRenderer3D::Render(const Camera3D& camera, TileCache& cache,
                                ID3D11RenderTargetView* rtv,
                                ID3D11DepthStencilView* dsv,
                                NavmeshColorMode colorMode,
                                bool drawEdges,
                                float heightMin, float heightMax) {
    if (!m_pipeline.IsReady() || !m_device || !m_context) return;

    SyncGpuCache(cache);
    if (m_gpuCache.empty()) {
        static int sLogCount = 0;
        if (sLogCount++ < 5)
            LOG(WARNING) << "[NavmeshRenderer3D] GPU cache empty (cache tiles="
                         << cache.GetLoadedCount() << ")";
        return;
    }

    ID3D11DeviceContext* ctx = m_context;

    // Bind render target with depth
    ctx->OMSetRenderTargets(1, &rtv, dsv);

    // Viewport (match camera viewport)
    D3D11_VIEWPORT vp = {};
    vp.TopLeftX = camera.vpX;
    vp.TopLeftY = camera.vpY;
    vp.Width    = camera.vpW;
    vp.Height   = camera.vpH;
    vp.MinDepth = 0.0f;
    vp.MaxDepth = 1.0f;
    ctx->RSSetViewports(1, &vp);

    // Pipeline state
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->VSSetShader(m_pipeline.GetVS(), nullptr, 0);
    ctx->PSSetShader(m_pipeline.GetPS(), nullptr, 0);
    ctx->IASetInputLayout(m_pipeline.GetInputLayout());

    float bf[4] = {0, 0, 0, 0};
    ctx->OMSetBlendState(m_pipeline.GetBlendState(), bf, 0xFFFFFFFF);
    ctx->RSSetState(m_pipeline.GetRastState());
    ctx->OMSetDepthStencilState(m_pipeline.GetDSState(), 0);

    // Prepare constant buffer (shared fields for all modes)
    NavmeshCB3D cb = {};
    memcpy(cb.viewProj, &camera.viewProj, sizeof(float) * 16);

    // Fill color (green, slightly transparent)
    cb.fillColor[0] = 0.0f;
    cb.fillColor[1] = 0.7f;
    cb.fillColor[2] = 0.3f;
    cb.fillColor[3] = 0.45f;  // semi-transparent overlay on terrain

    // Edge color (brighter green)
    cb.edgeColor[0] = 0.0f;
    cb.edgeColor[1] = 0.85f;
    cb.edgeColor[2] = 0.4f;
    cb.edgeColor[3] = 1.0f;

    // Light direction (from above-right, normalized)
    float lx = -0.5f, ly = -0.3f, lz = 0.8f;
    float len = sqrtf(lx * lx + ly * ly + lz * lz);
    cb.lightDir[0] = lx / len;
    cb.lightDir[1] = ly / len;
    cb.lightDir[2] = lz / len;
    cb.lightDir[3] = 0.0f;

    // Color mode parameters
    cb.colorParams[0] = static_cast<float>(static_cast<int>(colorMode));
    cb.colorParams[1] = heightMin;
    cb.colorParams[2] = heightMax;
    cb.colorParams[3] = drawEdges ? 1.0f : 0.0f;

    // tileHue defaults to 0 (set per-tile for TileColored mode)
    cb.tileHue[0] = 0.0f;
    cb.tileHue[1] = 0.0f;
    cb.tileHue[2] = 0.0f;
    cb.tileHue[3] = 0.0f;

    ID3D11Buffer* cbBuf = m_pipeline.GetConstantBuffer();

    // For non-TileColored modes, upload CB once before the draw loop
    bool isTileColored = (colorMode == NavmeshColorMode::TileColored);
    if (!isTileColored) {
        D3D11_MAPPED_SUBRESOURCE mapped = {};
        if (SUCCEEDED(ctx->Map(cbBuf, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
            memcpy(mapped.pData, &cb, sizeof(cb));
            ctx->Unmap(cbBuf, 0);
        }
    }

    ctx->VSSetConstantBuffers(0, 1, &cbBuf);
    ctx->PSSetConstantBuffers(0, 1, &cbBuf);

    // Frustum cull + draw visible tiles
    float frustum[6][4];
    camera.GetFrustumPlanes(frustum);

    UINT stride = sizeof(NavmeshVertex3D);
    UINT offset = 0;

    statDrawCalls = 0;
    statVertices  = 0;

    for (const auto& [key, tile] : m_gpuCache) {
        if (!FrustumIntersectsAABB(frustum, tile)) continue;

        // LOD: use detail for near tiles, base for far ones
        float dx = (tile.minX + tile.maxX) * 0.5f - camera.targetX;
        float dy = (tile.minY + tile.maxY) * 0.5f - camera.targetY;
        float dist = sqrtf(dx * dx + dy * dy);

        ID3D11Buffer* vb;
        UINT vc;
        if (dist < 400.0f) {
            vb = tile.detailVB;
            vc = tile.detailVertCount;
        } else {
            vb = tile.baseVB;
            vc = tile.baseVertCount;
        }

        // Fallback to detail if base unavailable
        if (!vb || vc == 0) { vb = tile.detailVB; vc = tile.detailVertCount; }
        if (!vb || vc == 0) continue;

        // For TileColored mode, compute a unique hue per tile and re-map the CB
        if (isTileColored) {
            cb.tileHue[0] = std::fmod(key.first * 0.1f + key.second * 0.37f, 1.0f);
            // Ensure hue is positive
            if (cb.tileHue[0] < 0.0f) cb.tileHue[0] += 1.0f;

            D3D11_MAPPED_SUBRESOURCE mapped = {};
            if (SUCCEEDED(ctx->Map(cbBuf, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
                memcpy(mapped.pData, &cb, sizeof(cb));
                ctx->Unmap(cbBuf, 0);
            }
        }

        ctx->IASetVertexBuffers(0, 1, &vb, &stride, &offset);
        ctx->Draw(vc, 0);
        statDrawCalls++;
        statVertices += vc;
    }

    // Unbind DSV for subsequent ImGui rendering
    ctx->OMSetRenderTargets(1, &rtv, nullptr);
}

} // namespace mapedit
