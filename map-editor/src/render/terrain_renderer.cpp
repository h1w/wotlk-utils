#include "terrain_renderer.h"
#include "../camera/camera3d.h"
#include "../mpq/mpq_archive.h"

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
// MPQ / texture configuration
// ---------------------------------------------------------------------------

void TerrainRenderer::SetMpqArchive(MpqArchiveSet* mpq) {
    std::lock_guard<std::mutex> lock(m_texMutex);
    if (m_mpq != mpq) {
        m_mpq = mpq;
        if (mpq)
            m_adtParser.Initialize(mpq);
    }
}

void TerrainRenderer::SetMapName(const std::string& name) {
    std::lock_guard<std::mutex> lock(m_texMutex);
    m_mapName = name;
}

void TerrainRenderer::SetTexturesEnabled(bool enabled) {
    std::lock_guard<std::mutex> lock(m_texMutex);
    m_texturesEnabled = enabled;
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

        result.decimation = req.decimation;

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

        // --- Texture compositing (if requested) ---
        if (result.valid && req.loadTextures && !req.mapName.empty()) {
            std::lock_guard<std::mutex> lock(m_texMutex);
            if (m_mpq && m_mpq->IsOpen()) {
                WdtInfo wdt = m_adtParser.ReadWdtMphd(req.mapName);
                AdtTextureData adt = m_adtParser.Parse(req.mapName, req.tileX, req.tileY,
                                                        wdt.mphdFlags);
                if (adt.valid && !adt.texturePaths.empty()) {
                    m_compositor.SetBlpCache(&m_blpCache);
                    auto bgra = m_compositor.CompositeTileAtlas(adt, wdt.mphdFlags, *m_mpq);
                    if (!bgra.empty()) {
                        result.compressedAtlas = CompressToBC1WithMips(bgra.data(), 1024, 1024);
                        result.hasTexture = !result.compressedAtlas.bc1Data.empty();
                    }
                }
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
    if (!m_texPipeline.Initialize(device)) {
        LOG(ERROR) << "[TerrainRenderer] Texture pipeline init failed";
        return false;
    }
    // Create shared Texture2DArray for tile textures (256 slots, BC1, 11 mips for 1024x1024)
    {
        static constexpr uint32_t kTexArrayCapacity = 64;
        static constexpr uint32_t kTexArrayMips = 11; // log2(1024) + 1

        D3D11_TEXTURE2D_DESC td = {};
        td.Width  = 1024;
        td.Height = 1024;
        td.MipLevels = kTexArrayMips;
        td.ArraySize = kTexArrayCapacity;
        td.Format    = DXGI_FORMAT_BC1_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage     = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;

        HRESULT hr = device->CreateTexture2D(&td, nullptr, &m_texArray.texture);
        if (FAILED(hr)) {
            LOG(ERROR) << "[TerrainRenderer] Failed to create texture array: 0x" << std::hex << hr;
            return false;
        }

        D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
        srvDesc.Format = DXGI_FORMAT_BC1_UNORM;
        srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
        srvDesc.Texture2DArray.MostDetailedMip = 0;
        srvDesc.Texture2DArray.MipLevels = kTexArrayMips;
        srvDesc.Texture2DArray.FirstArraySlice = 0;
        srvDesc.Texture2DArray.ArraySize = kTexArrayCapacity;

        hr = device->CreateShaderResourceView(m_texArray.texture, &srvDesc, &m_texArray.srv);
        if (FAILED(hr)) {
            LOG(ERROR) << "[TerrainRenderer] Failed to create texture array SRV: 0x" << std::hex << hr;
            m_texArray.texture->Release();
            m_texArray.texture = nullptr;
            return false;
        }

        m_texArray.capacity = kTexArrayCapacity;
        m_texArray.mipCount = kTexArrayMips;
        m_texArray.freeSlots.resize(kTexArrayCapacity);
        for (uint32_t i = 0; i < kTexArrayCapacity; ++i)
            m_texArray.freeSlots[i] = static_cast<int>(kTexArrayCapacity - 1 - i); // stack order

        LOG(INFO) << "[TerrainRenderer] Texture array created: " << kTexArrayCapacity << " slots, BC1, " << kTexArrayMips << " mips";
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
    m_texArray.Release();
    m_pipeline.Shutdown();
    m_texPipeline.Shutdown();
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
    if (t.textureSlot >= 0) {
        m_texArray.FreeSlot(t.textureSlot);
        t.textureSlot = -1;
    }
    if (t.vb) { t.vb->Release(); t.vb = nullptr; }
    if (t.ib) { t.ib->Release(); t.ib = nullptr; }
    t.hasTexture = false;
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

    // Upload BC1-compressed texture atlas into shared texture array
    if (result.hasTexture && !result.compressedAtlas.bc1Data.empty() && m_texArray.texture) {
        const auto& atlas = result.compressedAtlas;
        int slot = m_texArray.AllocateSlot();

        if (slot >= 0 && atlas.mipCount <= m_texArray.mipCount) {
            uint32_t mw = atlas.width, mh = atlas.height;
            for (uint32_t mip = 0; mip < atlas.mipCount; ++mip) {
                UINT subresource = D3D11CalcSubresource(mip, static_cast<UINT>(slot), m_texArray.mipCount);
                uint32_t blocksX = (std::max)(1u, (mw + 3) / 4);
                uint32_t blocksY = (std::max)(1u, (mh + 3) / 4);
                uint32_t rowPitch = blocksX * 8;  // BC1: 8 bytes per 4x4 block

                D3D11_BOX box = {};
                box.left   = 0;
                box.top    = 0;
                box.front  = 0;
                box.right  = mw;
                box.bottom = mh;
                box.back   = 1;

                m_context->UpdateSubresource(m_texArray.texture, subresource, &box,
                                             atlas.bc1Data.data() + atlas.mipOffsets[mip],
                                             rowPitch, 0);

                mw = (std::max)(1u, mw / 2);
                mh = (std::max)(1u, mh / 2);
            }
            gpu.textureSlot = slot;
            gpu.hasTexture = true;
        } else if (slot >= 0) {
            // Mip count mismatch — return slot
            m_texArray.FreeSlot(slot);
        }
    }

    gpu.indexCount = static_cast<UINT>(result.indices.size());
    gpu.minX = result.minX; gpu.minY = result.minY; gpu.minZ = result.minZ;
    gpu.maxX = result.maxX; gpu.maxY = result.maxY; gpu.maxZ = result.maxZ;
    gpu.decimation = result.decimation;

    m_globalMinZ = (std::min)(m_globalMinZ, result.minZ);
    m_globalMaxZ = (std::max)(m_globalMaxZ, result.maxZ);

    // Release old tile GPU resources if upgrading LOD
    TileKey key = {result.tileX, result.tileY};
    auto it = m_gpuCache.find(key);
    if (it != m_gpuCache.end())
        ReleaseTileGpu(it->second);

    m_gpuCache[key] = gpu;
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

            // Skip if cache already has equal or better LOD
            auto cacheIt = m_gpuCache.find(key);
            if (cacheIt != m_gpuCache.end() &&
                cacheIt->second.decimation > 0 &&
                cacheIt->second.decimation <= result.decimation)
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

    // Texture loading context snapshot
    bool texEnabled = false;
    std::string mapName;
    {
        std::lock_guard<std::mutex> lock(m_texMutex);
        texEnabled = m_texturesEnabled && m_mpq != nullptr;
        mapName = m_mapName;
    }

    int queued = 0;
    static constexpr int kMaxQueuesPerFrame = 8;
    for (const auto& td : desired) {
        if (queued >= kMaxQueuesPerFrame) break;
        TileKey key = {td.tx, td.ty};

        // Select LOD based on distance from camera center
        int lod = SelectLOD(td.dist);

        // Skip if already pending at equal or better LOD
        auto pendIt = m_pending.find(key);
        if (pendIt != m_pending.end() && pendIt->second <= lod)
            continue;

        // Skip if cache already has equal or better LOD
        auto cacheIt = m_gpuCache.find(key);
        if (cacheIt != m_gpuCache.end()) {
            bool needsTexture = texEnabled && !cacheIt->second.hasTexture && td.dist < 9.0f;
            if (cacheIt->second.decimation > 0 &&
                cacheIt->second.decimation <= lod &&
                !needsTexture)
                continue;
            // Empty sentinel (decimation=0) means no data — skip
            if (cacheIt->second.decimation == 0 && cacheIt->second.vb == nullptr)
                continue;
        }

        LoadRequest req;
        req.mapId = mapId;
        req.tileX = td.tx;
        req.tileY = td.ty;
        req.decimation = lod;
        req.dataPath = currentPath;
        req.loadTextures = texEnabled && td.dist < 9.0f;  // only close tiles
        req.mapName = mapName;

        {
            std::lock_guard<std::mutex> lock(m_reqMutex);
            m_requests.push_back(std::move(req));
        }
        m_reqCV.notify_one();

        m_pending[key] = lod;
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

    // Check if textures are enabled
    bool texEnabled;
    {
        std::lock_guard<std::mutex> lock(m_texMutex);
        texEnabled = m_texturesEnabled;
    }

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

    // Use textured pipeline for terrain (handles both textured and procedural)
    bool useTexPipeline = m_texPipeline.IsReady();
    auto& pipe = useTexPipeline
        ? static_cast<TerrainTexturePipeline&>(m_texPipeline)
        : m_texPipeline;  // fallback — won't happen if init succeeds

    if (useTexPipeline) {
        ctx->VSSetShader(m_texPipeline.GetVS(), nullptr, 0);
        ctx->PSSetShader(m_texPipeline.GetPS(), nullptr, 0);
        ctx->IASetInputLayout(m_texPipeline.GetLayout());
        float bf[4] = {0, 0, 0, 0};
        ctx->OMSetBlendState(m_texPipeline.GetBlendState(), bf, 0xFFFFFFFF);
        ctx->RSSetState(m_texPipeline.GetRastState());
        ctx->OMSetDepthStencilState(m_texPipeline.GetDSState(), 0);
    } else {
        ctx->VSSetShader(m_pipeline.GetVS(), nullptr, 0);
        ctx->PSSetShader(m_pipeline.GetPS(), nullptr, 0);
        ctx->IASetInputLayout(m_pipeline.GetLayout());
        float bf[4] = {0, 0, 0, 0};
        ctx->OMSetBlendState(m_pipeline.GetBlendState(), bf, 0xFFFFFFFF);
        ctx->RSSetState(m_pipeline.GetRastState());
        ctx->OMSetDepthStencilState(m_pipeline.GetDSState(), 0);
    }

    // Constant buffer (common for all tiles — per-tile colorMode override below)
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
    cb.baseColor[3] = smoothTerrain ? -1.0f : 0.0f;

    cb.heightParams[0] = m_globalMinZ;
    cb.heightParams[1] = m_globalMaxZ;
    cb.heightParams[3] = smoothTerrain ? -1.0f : -2.0f;

    ID3D11Buffer* cbBuf = useTexPipeline ? m_texPipeline.GetCB() : m_pipeline.GetCB();

    float frustum[6][4];
    camera.GetFrustumPlanes(frustum);

    UINT stride = sizeof(TerrainVertexGpu);
    UINT offset = 0;

    statDrawCalls = 0;
    statVertices  = 0;

    // Bind sampler + texture array once
    if (useTexPipeline) {
        ID3D11SamplerState* sam = m_texPipeline.GetSampler();
        ctx->PSSetSamplers(0, 1, &sam);

        // Bind shared texture array SRV once for all tiles
        if (m_texArray.srv) {
            ctx->PSSetShaderResources(0, 1, &m_texArray.srv);
        }
    }

    // Partition visible tiles into procedural (single CB) and textured (per-tile CB)
    struct VisibleTile { const TileGpu* tile; bool textured; };
    std::vector<VisibleTile> visibleTiles;
    visibleTiles.reserve(m_gpuCache.size());

    for (const auto& [key, tile] : m_gpuCache) {
        if (!tile.vb || !tile.ib || tile.indexCount == 0) continue;
        if (!FrustumIntersectsAABB(frustum, tile)) continue;
        bool textured = texEnabled && tile.hasTexture && useTexPipeline && tile.textureSlot >= 0;
        visibleTiles.push_back({ &tile, textured });
    }

    // Bind CB once for all tiles
    ctx->VSSetConstantBuffers(0, 1, &cbBuf);
    ctx->PSSetConstantBuffers(0, 1, &cbBuf);

    // Draw procedural tiles first (single CB update for batch)
    {
        cb.heightParams[2] = static_cast<float>(colorMode);
        cb.tileParams[0] = 0.0f;
        bool cbUpdated = false;

        for (const auto& vt : visibleTiles) {
            if (vt.textured) continue;

            if (!cbUpdated) {
                D3D11_MAPPED_SUBRESOURCE mapped = {};
                if (SUCCEEDED(ctx->Map(cbBuf, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
                    std::memcpy(mapped.pData, &cb, sizeof(cb));
                    ctx->Unmap(cbBuf, 0);
                }
                cbUpdated = true;
            }

            ctx->IASetVertexBuffers(0, 1, &vt.tile->vb, &stride, &offset);
            ctx->IASetIndexBuffer(vt.tile->ib, DXGI_FORMAT_R32_UINT, 0);
            ctx->DrawIndexed(vt.tile->indexCount, 0, 0);
            statDrawCalls++;
            statVertices += vt.tile->indexCount;
        }
    }

    // Draw textured tiles (per-tile CB update for texture slot index only)
    {
        cb.heightParams[2] = 3.0f;
        for (const auto& vt : visibleTiles) {
            if (!vt.textured) continue;

            cb.tileParams[0] = static_cast<float>(vt.tile->textureSlot);
            {
                D3D11_MAPPED_SUBRESOURCE mapped = {};
                if (SUCCEEDED(ctx->Map(cbBuf, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
                    std::memcpy(mapped.pData, &cb, sizeof(cb));
                    ctx->Unmap(cbBuf, 0);
                }
            }

            ctx->IASetVertexBuffers(0, 1, &vt.tile->vb, &stride, &offset);
            ctx->IASetIndexBuffer(vt.tile->ib, DXGI_FORMAT_R32_UINT, 0);
            ctx->DrawIndexed(vt.tile->indexCount, 0, 0);
            statDrawCalls++;
            statVertices += vt.tile->indexCount;
        }
    }

    // Unbind SRV to avoid warnings
    if (useTexPipeline) {
        ID3D11ShaderResourceView* nullSrv = nullptr;
        ctx->PSSetShaderResources(0, 1, &nullSrv);
    }

    ctx->OMSetRenderTargets(1, &rtv, nullptr);
}

} // namespace mapedit
