#include "building_renderer.h"
#include "../camera/camera3d.h"

#include <glog/logging.h>
#include <DirectXMath.h>

#include <algorithm>
#include <cstring>
#include <cmath>

namespace mapedit {

static constexpr float TILE_SIZE = 533.33333f;
static constexpr float VMAP_MID = 0.5f * 64.0f * TILE_SIZE;  // 17066.666f
static constexpr float PI = 3.14159265358979323846f;

// Small object culling: skip M2 models with bounding box < this size (yards)
// when camera distance exceeds the threshold
static constexpr float kSmallObjectSize = 3.0f;
static constexpr float kSmallObjectCullDist = 500.0f;
static constexpr uint32_t MOD_M2 = 0x01;

// ---------------------------------------------------------------------------
// Frustum culling
// ---------------------------------------------------------------------------

bool BuildingRenderer::FrustumIntersectsAABB(const float planes[6][4],
                                               const TileBuildings& tile) {
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
// Transform model vertices to WoW world coordinates (free function for worker)
// ---------------------------------------------------------------------------

static void TransformVertices(const BuildingMesh& mesh, const ModelSpawn& spawn,
                               std::vector<float>& outVerts,
                               std::vector<uint32_t>& outIndices,
                               float bounds[6]) {
    if (mesh.vertices.empty() || mesh.indices.empty())
        return;

    size_t nVerts = mesh.vertices.size() / 3;
    uint32_t vertexBase = static_cast<uint32_t>(outVerts.size() / 3);

    // Build rotation matrix from Euler angles (ZYX order, degrees)
    float yawRad   = spawn.rotY * PI / 180.0f;
    float pitchRad = spawn.rotX * PI / 180.0f;
    float rollRad  = spawn.rotZ * PI / 180.0f;

    float cy = cosf(yawRad),  sy = sinf(yawRad);
    float cp = cosf(pitchRad), sp = sinf(pitchRad);
    float cr = cosf(rollRad),  sr = sinf(rollRad);

    float r00 = cy * cp;
    float r01 = cy * sp * sr - sy * cr;
    float r02 = cy * sp * cr + sy * sr;
    float r10 = sy * cp;
    float r11 = sy * sp * sr + cy * cr;
    float r12 = sy * sp * cr - cy * sr;
    float r20 = -sp;
    float r21 = cp * sr;
    float r22 = cp * cr;

    float scale = spawn.scale;

    outVerts.reserve(outVerts.size() + nVerts * 3);

    for (size_t v = 0; v < nVerts; ++v) {
        float mx = mesh.vertices[v * 3 + 0] * scale;
        float my = mesh.vertices[v * 3 + 1] * scale;
        float mz = mesh.vertices[v * 3 + 2] * scale;

        float rx = r00 * mx + r01 * my + r02 * mz;
        float ry = r10 * mx + r11 * my + r12 * mz;
        float rz = r20 * mx + r21 * my + r22 * mz;

        float wowX = VMAP_MID - (spawn.posX + rx);
        float wowY = VMAP_MID - (spawn.posY + ry);
        float wowZ = spawn.posZ + rz;

        outVerts.push_back(wowX);
        outVerts.push_back(wowY);
        outVerts.push_back(wowZ);

        bounds[0] = (std::min)(bounds[0], wowX);
        bounds[1] = (std::min)(bounds[1], wowY);
        bounds[2] = (std::min)(bounds[2], wowZ);
        bounds[3] = (std::max)(bounds[3], wowX);
        bounds[4] = (std::max)(bounds[4], wowY);
        bounds[5] = (std::max)(bounds[5], wowZ);
    }

    outIndices.reserve(outIndices.size() + mesh.indices.size());
    for (uint32_t idx : mesh.indices)
        outIndices.push_back(vertexBase + idx);
}

// ---------------------------------------------------------------------------
// Worker thread
// ---------------------------------------------------------------------------

void BuildingRenderer::StartWorker() {
    m_running = true;
    m_worker = std::thread(&BuildingRenderer::WorkerLoop, this);
}

void BuildingRenderer::StopWorker() {
    {
        std::lock_guard<std::mutex> lock(m_reqMutex);
        m_running = false;
    }
    m_reqCV.notify_all();
    if (m_worker.joinable())
        m_worker.join();
}

void BuildingRenderer::WorkerLoop() {
    // Worker owns its own loader instances (no sharing with main thread)
    BuildingLoader buildingLoader;
    VMapTileLoader vmapLoader;
    uint32_t workerMapId = UINT32_MAX;

    while (m_running) {
        LoadRequest req;
        {
            std::unique_lock<std::mutex> lock(m_reqMutex);
            m_reqCV.wait(lock, [&] { return !m_requests.empty() || !m_running; });
            if (!m_running) break;
            req = std::move(m_requests.front());
            m_requests.pop_front();
        }

        // Update loaders if data path or map changed
        buildingLoader.SetDataPath(req.dataPath);
        vmapLoader.SetDataPath(req.dataPath);

        if (req.mapId != workerMapId) {
            buildingLoader.ClearCache();
            workerMapId = req.mapId;
        }

        // Load vmtile + building geometry (CPU-heavy work)
        LoadResult result;
        result.mapId = req.mapId;
        result.tileX = req.tileX;
        result.tileY = req.tileY;
        result.empty = true;
        std::fill(std::begin(result.bounds), std::end(result.bounds), 0.0f);

        VMapTileData vmapData;
        if (vmapLoader.LoadTile(req.mapId, req.tileX, req.tileY, vmapData) &&
            !vmapData.spawns.empty()) {

            std::vector<float> mergedVerts;
            std::vector<uint32_t> mergedIndices;
            float bounds[6] = { 1e30f, 1e30f, 1e30f, -1e30f, -1e30f, -1e30f };

            int loadedModels = 0;
            for (const auto& spawn : vmapData.spawns) {
                const BuildingMesh* mesh = buildingLoader.LoadBuilding(spawn.modelName);
                if (!mesh)
                    continue;

                // Small object culling: skip small M2 models when zoomed out
                if (req.cameraDistance > kSmallObjectCullDist && (spawn.flags & MOD_M2)) {
                    float dx = mesh->bounds[3] - mesh->bounds[0];
                    float dy = mesh->bounds[4] - mesh->bounds[1];
                    float dz = mesh->bounds[5] - mesh->bounds[2];
                    float maxDim = (std::max)({dx, dy, dz});
                    if (maxDim * spawn.scale < kSmallObjectSize)
                        continue;
                }

                TransformVertices(*mesh, spawn, mergedVerts, mergedIndices, bounds);
                ++loadedModels;
            }

            if (!mergedVerts.empty() && !mergedIndices.empty()) {
                // Convert to GPU vertex format
                size_t nVerts = mergedVerts.size() / 3;
                result.vertices.resize(nVerts);
                for (size_t i = 0; i < nVerts; ++i) {
                    result.vertices[i].x  = mergedVerts[i * 3 + 0];
                    result.vertices[i].y  = mergedVerts[i * 3 + 1];
                    result.vertices[i].z  = mergedVerts[i * 3 + 2];
                    result.vertices[i].nx = 0.0f;
                    result.vertices[i].ny = 0.0f;
                    result.vertices[i].nz = 1.0f;
                }
                result.indices = std::move(mergedIndices);
                std::memcpy(result.bounds, bounds, sizeof(bounds));
                result.empty = false;

                LOG(INFO) << "[BuildingRenderer] Tile (" << req.tileX << "," << req.tileY
                          << "): " << loadedModels << " models, "
                          << nVerts << " verts, "
                          << (result.indices.size() / 3) << " tris";
            }
        }

        // Post result
        {
            std::lock_guard<std::mutex> lock(m_resMutex);
            m_results.push_back(std::move(result));
        }
    }
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

bool BuildingRenderer::Initialize(ID3D11Device* device, ID3D11DeviceContext* context) {
    m_device  = device;
    m_context = context;
    if (!m_pipeline.Initialize(device)) {
        LOG(ERROR) << "[BuildingRenderer] Pipeline init failed";
        return false;
    }
    StartWorker();
    LOG(INFO) << "[BuildingRenderer] GPU pipeline ready (background loading enabled)";
    return true;
}

void BuildingRenderer::Shutdown() {
    StopWorker();
    for (auto& [key, tile] : m_gpuCache)
        ReleaseTileGpu(tile);
    m_gpuCache.clear();
    m_pending.clear();
    m_pipeline.Shutdown();
    m_device  = nullptr;
    m_context = nullptr;
}

void BuildingRenderer::SetDataPath(const std::string& tcDataPath) {
    std::lock_guard<std::mutex> lock(m_pathMutex);
    m_dataPath = tcDataPath;
}

void BuildingRenderer::ReleaseTileGpu(TileBuildings& t) {
    if (t.vb) { t.vb->Release(); t.vb = nullptr; }
    if (t.ib) { t.ib->Release(); t.ib = nullptr; }
}

bool BuildingRenderer::UploadToGpu(const LoadResult& result) {
    if (result.vertices.empty() || result.indices.empty())
        return false;

    TileBuildings gpu;

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
    gpu.minX = result.bounds[0]; gpu.minY = result.bounds[1]; gpu.minZ = result.bounds[2];
    gpu.maxX = result.bounds[3]; gpu.maxY = result.bounds[4]; gpu.maxZ = result.bounds[5];

    m_globalMinZ = (std::min)(m_globalMinZ, result.bounds[2]);
    m_globalMaxZ = (std::max)(m_globalMaxZ, result.bounds[5]);

    m_gpuCache[{result.tileX, result.tileY}] = gpu;
    return true;
}

// ---------------------------------------------------------------------------
// Viewport update
// ---------------------------------------------------------------------------

void BuildingRenderer::UpdateViewport(uint32_t mapId, float targetX, float targetY,
                                       float cameraDistance) {
    if (mapId != m_currentMapId) {
        {
            std::lock_guard<std::mutex> lock(m_reqMutex);
            m_requests.clear();
        }
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

    // --- Process completed background loads ---
    {
        std::lock_guard<std::mutex> lock(m_resMutex);
        int uploads = 0;
        while (!m_results.empty() && uploads < kMaxUploadsPerFrame) {
            auto result = std::move(m_results.front());
            m_results.pop_front();

            TileKey key = {result.tileX, result.tileY};
            m_pending.erase(key);

            if (result.mapId != m_currentMapId)
                continue;

            if (m_gpuCache.count(key))
                continue;

            if (!result.empty) {
                UploadToGpu(result);
                ++uploads;
            } else {
                // Empty sentinel
                m_gpuCache[key] = TileBuildings{};
            }
        }
    }

    // --- Calculate visible tile range ---
    float viewRadius = cameraDistance * 1.2f;
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

        int toEvict = static_cast<int>(m_gpuCache.size()) - kMaxCachedTiles + 10;
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

    // --- Queue new tile loads ---
    std::string currentPath;
    {
        std::lock_guard<std::mutex> lock(m_pathMutex);
        currentPath = m_dataPath;
    }

    int queued = 0;
    static constexpr int kMaxQueuesPerFrame = 4;
    for (const auto& td : desired) {
        if (queued >= kMaxQueuesPerFrame) break;
        TileKey key = {td.tx, td.ty};

        if (m_gpuCache.count(key)) continue;
        if (m_pending.count(key)) continue;

        LoadRequest req;
        req.mapId = mapId;
        req.tileX = td.tx;
        req.tileY = td.ty;
        req.dataPath = currentPath;
        req.cameraDistance = cameraDistance;

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

void BuildingRenderer::Render(const Camera3D& camera,
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

    TerrainCB cb = {};
    std::memcpy(cb.viewProj, &camera.viewProj, sizeof(float) * 16);

    float lx = -0.5f, ly = -0.3f, lz = 0.8f;
    float len = std::sqrt(lx * lx + ly * ly + lz * lz);
    cb.lightDir[0] = lx / len;
    cb.lightDir[1] = ly / len;
    cb.lightDir[2] = lz / len;
    cb.lightDir[3] = 0.3f;

    cb.baseColor[0] = 0.50f;
    cb.baseColor[1] = 0.48f;
    cb.baseColor[2] = 0.45f;
    cb.baseColor[3] = 1.0f;

    cb.heightParams[0] = m_globalMinZ;
    cb.heightParams[1] = m_globalMaxZ;
    cb.heightParams[2] = 0.0f;  // Always solid grey for buildings
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
