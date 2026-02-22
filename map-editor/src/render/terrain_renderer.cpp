#include "terrain_renderer.h"
#include "../camera/camera3d.h"

#include <glog/logging.h>

#include <algorithm>
#include <cstring>
#include <cmath>

namespace mapedit {

static constexpr float TILE_SIZE = 533.33333f;

// ---------------------------------------------------------------------------
// Frustum culling
// ---------------------------------------------------------------------------

bool TerrainRenderer::FrustumIntersectsAABB(const float planes[6][4],
                                              const TileGpu& tile) {
    for (int i = 0; i < 6; ++i) {
        float px = (planes[i][0] >= 0) ? tile.maxX : tile.minX;
        float py = (planes[i][1] >= 0) ? tile.maxY : tile.minY;
        float pz = (planes[i][2] >= 0) ? tile.maxZ : tile.minZ;
        float d = planes[i][0] * px + planes[i][1] * py +
                  planes[i][2] * pz + planes[i][3];
        if (d < 0) return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// LOD selection based on tile distance from camera
// ---------------------------------------------------------------------------

int TerrainRenderer::SelectLOD(float tileDist) {
    if (tileDist < 2.0f)  return 1;   // close: full resolution
    if (tileDist < 5.0f)  return 2;   // mid: half resolution
    return 4;                           // far: quarter resolution
}

// ---------------------------------------------------------------------------
// Worker thread
// ---------------------------------------------------------------------------

void TerrainRenderer::StartWorker() {
    m_running = true;
    m_worker = std::thread(&TerrainRenderer::WorkerLoop, this);
}

void TerrainRenderer::StopWorker() {
    {
        std::lock_guard<std::mutex> lock(m_reqMutex);
        m_running = false;
    }
    m_reqCV.notify_all();
    if (m_worker.joinable())
        m_worker.join();
}

void TerrainRenderer::WorkerLoop() {
    // Worker has its own loader instance (no shared state with main thread)
    TerrainLoader loader;

    while (m_running) {
        LoadRequest req;
        {
            std::unique_lock<std::mutex> lock(m_reqMutex);
            m_reqCV.wait(lock, [&] { return !m_requests.empty() || !m_running; });
            if (!m_running) break;
            req = std::move(m_requests.front());
            m_requests.pop_front();
        }

        // Set data path (may change between requests)
        loader.SetDataPath(req.dataPath);

        // Load tile from disk + generate mesh (CPU-heavy work)
        LoadResult result;
        result.mapId = req.mapId;
        result.tileX = req.tileX;
        result.tileY = req.tileY;
        result.valid = false;

        TerrainTileData data;
        if (loader.LoadTile(req.mapId, req.tileX, req.tileY, data)) {
            TerrainMesh mesh;
            GenerateTerrainMesh(data, req.tileX, req.tileY, mesh, req.decimation);

            if (!mesh.vertices.empty() && !mesh.indices.empty()) {
                result.vertices = std::move(mesh.vertices);
                result.indices  = std::move(mesh.indices);
                result.minX = mesh.minX; result.minY = mesh.minY; result.minZ = mesh.minZ;
                result.maxX = mesh.maxX; result.maxY = mesh.maxY; result.maxZ = mesh.maxZ;
                result.valid = true;
            }
        }

        // Post result (even invalid ones, so main thread stops waiting)
        {
            std::lock_guard<std::mutex> lock(m_resMutex);
            m_results.push_back(std::move(result));
        }
    }
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

bool TerrainRenderer::Initialize(ID3D11Device* device, ID3D11DeviceContext* context) {
    m_device  = device;
    m_context = context;
    if (!m_pipeline.Initialize(device)) {
        LOG(ERROR) << "[TerrainRenderer] Pipeline init failed";
        return false;
    }
    StartWorker();
    LOG(INFO) << "[TerrainRenderer] GPU pipeline ready (background loading enabled)";
    return true;
}

void TerrainRenderer::Shutdown() {
    StopWorker();
    for (auto& [key, tile] : m_gpuCache)
        ReleaseTileGpu(tile);
    m_gpuCache.clear();
    m_pending.clear();
    m_pipeline.Shutdown();
    m_device  = nullptr;
    m_context = nullptr;
}

void TerrainRenderer::SetDataPath(const std::string& tcDataPath) {
    std::lock_guard<std::mutex> lock(m_pathMutex);
    m_dataPath = tcDataPath;
}

// ---------------------------------------------------------------------------
// GPU tile management
// ---------------------------------------------------------------------------

void TerrainRenderer::ReleaseTileGpu(TileGpu& t) {
    if (t.vb) { t.vb->Release(); t.vb = nullptr; }
    if (t.ib) { t.ib->Release(); t.ib = nullptr; }
}

bool TerrainRenderer::UploadToGpu(const LoadResult& result) {
    if (result.vertices.empty() || result.indices.empty())
        return false;

    TileGpu gpu;

    // Create vertex buffer
    {
        D3D11_BUFFER_DESC desc = {};
        desc.ByteWidth = static_cast<UINT>(result.vertices.size() * sizeof(TerrainVertexGpu));
        desc.Usage     = D3D11_USAGE_IMMUTABLE;
        desc.BindFlags = D3D11_BIND_VERTEX_BUFFER;

        D3D11_SUBRESOURCE_DATA init = {};
        init.pSysMem = result.vertices.data();

        if (FAILED(m_device->CreateBuffer(&desc, &init, &gpu.vb)))
            return false;
    }

    // Create index buffer
    {
        D3D11_BUFFER_DESC desc = {};
        desc.ByteWidth = static_cast<UINT>(result.indices.size() * sizeof(uint32_t));
        desc.Usage     = D3D11_USAGE_IMMUTABLE;
        desc.BindFlags = D3D11_BIND_INDEX_BUFFER;

        D3D11_SUBRESOURCE_DATA init = {};
        init.pSysMem = result.indices.data();

        if (FAILED(m_device->CreateBuffer(&desc, &init, &gpu.ib))) {
            gpu.vb->Release();
            return false;
        }
    }

    gpu.indexCount = static_cast<UINT>(result.indices.size());
    gpu.minX = result.minX; gpu.minY = result.minY; gpu.minZ = result.minZ;
    gpu.maxX = result.maxX; gpu.maxY = result.maxY; gpu.maxZ = result.maxZ;

    m_globalMinZ = (std::min)(m_globalMinZ, result.minZ);
    m_globalMaxZ = (std::max)(m_globalMaxZ, result.maxZ);

    m_gpuCache[{result.tileX, result.tileY}] = gpu;
    return true;
}

// ---------------------------------------------------------------------------
// Viewport update — queue tile loads and process completed results
// ---------------------------------------------------------------------------

void TerrainRenderer::UpdateViewport(uint32_t mapId, float targetX, float targetY,
                                      float cameraDistance) {
    // Map changed — flush everything
    if (mapId != m_currentMapId) {
        // Clear request queue
        {
            std::lock_guard<std::mutex> lock(m_reqMutex);
            m_requests.clear();
        }
        // Clear result queue
        {
            std::lock_guard<std::mutex> lock(m_resMutex);
            m_results.clear();
        }

        for (auto& [k, t] : m_gpuCache)
            ReleaseTileGpu(t);
        m_gpuCache.clear();
        m_pending.clear();
        m_currentMapId = mapId;
        m_globalMinZ =  1e30f;
        m_globalMaxZ = -1e30f;
    }

    // --- Process completed background loads (upload to GPU) ---
    {
        std::lock_guard<std::mutex> lock(m_resMutex);
        int uploads = 0;
        while (!m_results.empty() && uploads < kMaxUploadsPerFrame) {
            auto result = std::move(m_results.front());
            m_results.pop_front();

            TileKey key = {result.tileX, result.tileY};
            m_pending.erase(key);

            // Discard if map changed while loading
            if (result.mapId != m_currentMapId)
                continue;

            // Skip if already in cache (shouldn't happen, but guard)
            if (m_gpuCache.count(key))
                continue;

            if (result.valid) {
                UploadToGpu(result);
                ++uploads;
            } else {
                // Insert empty sentinel so we don't re-queue
                m_gpuCache[key] = TileGpu{};
            }
        }
    }

    // --- Calculate visible tile range ---
    float viewRadius = cameraDistance * 1.5f;
    float minX = targetX - viewRadius;
    float maxX = targetX + viewRadius;
    float minY = targetY - viewRadius;
    float maxY = targetY + viewRadius;

    auto worldToTile = [](float wowCoord) -> int {
        return static_cast<int>(31.0f - std::floor(wowCoord / TILE_SIZE));
    };

    int txMin = worldToTile(maxX);
    int txMax = worldToTile(minX);
    int tyMin = worldToTile(maxY);
    int tyMax = worldToTile(minY);

    txMin = (std::max)(0, txMin);
    txMax = (std::min)(63, txMax);
    tyMin = (std::max)(0, tyMin);
    tyMax = (std::min)(63, tyMax);

    int centerTX = worldToTile(targetX);
    int centerTY = worldToTile(targetY);

    // Collect desired tiles sorted by distance to center
    struct TileDist {
        int tx, ty;
        float dist;
    };
    std::vector<TileDist> desired;
    for (int tx = txMin; tx <= txMax; ++tx) {
        for (int ty = tyMin; ty <= tyMax; ++ty) {
            float dtx = static_cast<float>(tx - centerTX);
            float dty = static_cast<float>(ty - centerTY);
            desired.push_back({tx, ty, dtx * dtx + dty * dty});
        }
    }
    std::sort(desired.begin(), desired.end(),
              [](const TileDist& a, const TileDist& b) { return a.dist < b.dist; });

    // --- Evict distant tiles if cache is large ---
    if (static_cast<int>(m_gpuCache.size()) > kMaxCachedTiles) {
        std::vector<std::pair<TileKey, float>> cached;
        for (const auto& [key, tile] : m_gpuCache) {
            float dtx = static_cast<float>(key.first - centerTX);
            float dty = static_cast<float>(key.second - centerTY);
            cached.push_back({key, dtx * dtx + dty * dty});
        }
        std::sort(cached.begin(), cached.end(),
                  [](const auto& a, const auto& b) { return a.second > b.second; });

        int toEvict = static_cast<int>(m_gpuCache.size()) - kMaxCachedTiles + 20;
        for (int i = 0; i < toEvict && i < static_cast<int>(cached.size()); ++i) {
            auto it = m_gpuCache.find(cached[i].first);
            if (it != m_gpuCache.end()) {
                ReleaseTileGpu(it->second);
                m_gpuCache.erase(it);
            }
        }

        m_globalMinZ =  1e30f;
        m_globalMaxZ = -1e30f;
        for (const auto& [key, tile] : m_gpuCache) {
            m_globalMinZ = (std::min)(m_globalMinZ, tile.minZ);
            m_globalMaxZ = (std::max)(m_globalMaxZ, tile.maxZ);
        }
    }

    // --- Queue new tile loads (throttled) ---
    std::string currentPath;
    {
        std::lock_guard<std::mutex> lock(m_pathMutex);
        currentPath = m_dataPath;
    }

    int queued = 0;
    static constexpr int kMaxQueuesPerFrame = 8;
    for (const auto& td : desired) {
        if (queued >= kMaxQueuesPerFrame) break;
        TileKey key = {td.tx, td.ty};

        // Skip if already cached or pending
        if (m_gpuCache.count(key)) continue;
        if (m_pending.count(key)) continue;

        // Select LOD based on distance from camera center
        int lod = SelectLOD(td.dist);

        LoadRequest req;
        req.mapId = mapId;
        req.tileX = td.tx;
        req.tileY = td.ty;
        req.decimation = lod;
        req.dataPath = currentPath;

        {
            std::lock_guard<std::mutex> lock(m_reqMutex);
            m_requests.push_back(std::move(req));
        }
        m_reqCV.notify_one();

        m_pending.insert(key);
        ++queued;
    }
}

// ---------------------------------------------------------------------------
// Render
// ---------------------------------------------------------------------------

void TerrainRenderer::Render(const Camera3D& camera,
                              ID3D11RenderTargetView* rtv,
                              ID3D11DepthStencilView* dsv) {
    if (!m_pipeline.IsReady() || !m_device || !m_context) return;
    if (m_gpuCache.empty()) return;

    ID3D11DeviceContext* ctx = m_context;

    ctx->OMSetRenderTargets(1, &rtv, dsv);

    D3D11_VIEWPORT vp = {};
    vp.TopLeftX = camera.vpX;
    vp.TopLeftY = camera.vpY;
    vp.Width    = camera.vpW;
    vp.Height   = camera.vpH;
    vp.MinDepth = 0.0f;
    vp.MaxDepth = 1.0f;
    ctx->RSSetViewports(1, &vp);

    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->VSSetShader(m_pipeline.GetVS(), nullptr, 0);
    ctx->PSSetShader(m_pipeline.GetPS(), nullptr, 0);
    ctx->IASetInputLayout(m_pipeline.GetLayout());

    float bf[4] = {0, 0, 0, 0};
    ctx->OMSetBlendState(m_pipeline.GetBlendState(), bf, 0xFFFFFFFF);
    ctx->RSSetState(m_pipeline.GetRastState());
    ctx->OMSetDepthStencilState(m_pipeline.GetDSState(), 0);

    // Constant buffer
    TerrainCB cb = {};
    std::memcpy(cb.viewProj, &camera.viewProj, sizeof(float) * 16);

    float lx = -0.5f, ly = -0.3f, lz = 0.8f;
    float len = std::sqrt(lx * lx + ly * ly + lz * lz);
    cb.lightDir[0] = lx / len;
    cb.lightDir[1] = ly / len;
    cb.lightDir[2] = lz / len;
    cb.lightDir[3] = 0.3f;  // ambient

    cb.baseColor[0] = 0.45f;
    cb.baseColor[1] = 0.45f;
    cb.baseColor[2] = 0.48f;
    cb.baseColor[3] = 1.0f;

    cb.heightParams[0] = m_globalMinZ;
    cb.heightParams[1] = m_globalMaxZ;
    cb.heightParams[2] = static_cast<float>(colorMode);
    cb.heightParams[3] = 0.0f;

    ID3D11Buffer* cbBuf = m_pipeline.GetCB();
    {
        D3D11_MAPPED_SUBRESOURCE mapped = {};
        if (SUCCEEDED(ctx->Map(cbBuf, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
            std::memcpy(mapped.pData, &cb, sizeof(cb));
            ctx->Unmap(cbBuf, 0);
        }
    }
    ctx->VSSetConstantBuffers(0, 1, &cbBuf);
    ctx->PSSetConstantBuffers(0, 1, &cbBuf);

    float frustum[6][4];
    camera.GetFrustumPlanes(frustum);

    UINT stride = sizeof(TerrainVertexGpu);
    UINT offset = 0;

    for (const auto& [key, tile] : m_gpuCache) {
        if (!tile.vb || !tile.ib || tile.indexCount == 0) continue;
        if (!FrustumIntersectsAABB(frustum, tile)) continue;

        ctx->IASetVertexBuffers(0, 1, &tile.vb, &stride, &offset);
        ctx->IASetIndexBuffer(tile.ib, DXGI_FORMAT_R32_UINT, 0);
        ctx->DrawIndexed(tile.indexCount, 0, 0);
    }

    ctx->OMSetRenderTargets(1, &rtv, nullptr);
}

} // namespace mapedit
