#include "building_renderer.h"
#include "../camera/camera3d.h"
#include "../data/wmo_portal_loader.h"
#include "../mpq/mpq_archive.h"

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

// MOGP group flags for portal culling
static constexpr uint32_t MOGP_EXTERIOR    = 0x8;      // outdoor group
static constexpr uint32_t MOGP_ALWAYSDRAW  = 0x10000;  // always visible (e.g. outer shell)
static constexpr uint32_t MOGP_INTERIOR    = 0x2000;   // indoor group (interior lit)
static constexpr int kMaxPortalBfsDepth = 12;

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
    WmoPortalLoader portalLoader;
    WmoVisualLoader wmoVisualLoader;
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
            portalLoader.ClearCache();
            wmoVisualLoader.ClearCache();
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

            bool showObjects = m_showObjects;

            std::vector<GroupDrawRange> groupRanges;
            std::vector<SpawnPortalInfo> spawnPortals;
            uint16_t spawnCounter = 0;

            int loadedModels = 0;
            for (const auto& spawn : vmapData.spawns) {
                // Skip M2 collision shapes (fences, poles, crates, etc.)
                // unless the "Objects" toggle is enabled
                if (!showObjects && (spawn.flags & MOD_M2))
                    continue;

                bool isMpqReady = m_mpq && m_mpq->IsOpen();
                bool isWmo = !(spawn.flags & MOD_M2);
                bool gotVisual = false;

                // --- Try WMO visual geometry from MPQ ---
                if (isWmo && isMpqReady) {
                    const WmoVisualData* visual = wmoVisualLoader.Load(spawn.modelName, *m_mpq);
                    if (visual && visual->valid && !visual->groups.empty()) {
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

                        for (uint16_t gi = 0; gi < static_cast<uint16_t>(visual->groups.size()); ++gi) {
                            const WmoGroupVisual& grp = visual->groups[gi];
                            if (grp.positions.empty() || grp.indices.empty())
                                continue;

                            size_t nVerts = grp.positions.size() / 3;
                            uint32_t vertexBase = static_cast<uint32_t>(mergedVerts.size() / 3);
                            uint32_t indexBase = static_cast<uint32_t>(mergedIndices.size());

                            for (size_t v = 0; v < nVerts; ++v) {
                                float mx = grp.positions[v * 3 + 0] * scale;
                                float my = grp.positions[v * 3 + 1] * scale;
                                float mz = grp.positions[v * 3 + 2] * scale;

                                float rx = r00 * mx + r01 * my + r02 * mz;
                                float ry = r10 * mx + r11 * my + r12 * mz;
                                float rz = r20 * mx + r21 * my + r22 * mz;

                                float wowX = VMAP_MID - (spawn.posX + rx);
                                float wowY = VMAP_MID - (spawn.posY + ry);
                                float wowZ = spawn.posZ + rz;

                                mergedVerts.push_back(wowX);
                                mergedVerts.push_back(wowY);
                                mergedVerts.push_back(wowZ);

                                bounds[0] = (std::min)(bounds[0], wowX);
                                bounds[1] = (std::min)(bounds[1], wowY);
                                bounds[2] = (std::min)(bounds[2], wowZ);
                                bounds[3] = (std::max)(bounds[3], wowX);
                                bounds[4] = (std::max)(bounds[4], wowY);
                                bounds[5] = (std::max)(bounds[5], wowZ);
                            }

                            for (uint32_t idx : grp.indices)
                                mergedIndices.push_back(vertexBase + idx);

                            GroupDrawRange range;
                            range.indexStart = indexBase;
                            range.indexCount = static_cast<uint32_t>(grp.indices.size());
                            range.mogpFlags = grp.mogpFlags;
                            range.spawnIdx = spawnCounter;
                            range.groupIdx = gi;

                            // Transform group bbox to world space (8-corner method)
                            float wMinX =  1e30f, wMinY =  1e30f, wMinZ =  1e30f;
                            float wMaxX = -1e30f, wMaxY = -1e30f, wMaxZ = -1e30f;

                            for (int corner = 0; corner < 8; ++corner) {
                                float lx = (corner & 1) ? grp.bbox[3] : grp.bbox[0];
                                float ly = (corner & 2) ? grp.bbox[4] : grp.bbox[1];
                                float lz = (corner & 4) ? grp.bbox[5] : grp.bbox[2];

                                float sx = lx * scale;
                                float sy2 = ly * scale;
                                float sz = lz * scale;

                                float bx = r00 * sx + r01 * sy2 + r02 * sz;
                                float by = r10 * sx + r11 * sy2 + r12 * sz;
                                float bz = r20 * sx + r21 * sy2 + r22 * sz;

                                float wowX = VMAP_MID - (spawn.posX + bx);
                                float wowY = VMAP_MID - (spawn.posY + by);
                                float wowZ = spawn.posZ + bz;

                                wMinX = (std::min)(wMinX, wowX);
                                wMinY = (std::min)(wMinY, wowY);
                                wMinZ = (std::min)(wMinZ, wowZ);
                                wMaxX = (std::max)(wMaxX, wowX);
                                wMaxY = (std::max)(wMaxY, wowY);
                                wMaxZ = (std::max)(wMaxZ, wowZ);
                            }

                            range.bboxWorld[0] = wMinX;
                            range.bboxWorld[1] = wMinY;
                            range.bboxWorld[2] = wMinZ;
                            range.bboxWorld[3] = wMaxX;
                            range.bboxWorld[4] = wMaxY;
                            range.bboxWorld[5] = wMaxZ;
                            range.wmoGroupId = gi;

                            groupRanges.push_back(range);
                        }
                        gotVisual = true;
                    }
                }

                // --- Fallback: TC collision geometry ---
                if (!gotVisual) {
                const BuildingMesh* mesh = buildingLoader.LoadBuilding(spawn.modelName);
                if (!mesh)
                    continue;

                // Record merged-index base before TransformVertices appends
                uint32_t indicesBaseBefore = static_cast<uint32_t>(mergedIndices.size());

                TransformVertices(*mesh, spawn, mergedVerts, mergedIndices, bounds);

                // Build per-group draw ranges if the mesh has group data
                if (!mesh->groups.empty()) {
                    // Compute rotation matrix (same as TransformVertices)
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

                    for (uint16_t gi = 0; gi < static_cast<uint16_t>(mesh->groups.size()); ++gi) {
                        const BuildingGroup& grp = mesh->groups[gi];

                        GroupDrawRange range;
                        range.indexStart = indicesBaseBefore + grp.indexOffset;
                        range.indexCount = grp.indexCount;
                        range.mogpFlags = grp.mogpFlags;
                        range.spawnIdx = spawnCounter;
                        range.groupIdx = gi;

                        // Transform model-local AABB to world-space AABB
                        // by transforming all 8 corners and taking min/max
                        float wMinX =  1e30f, wMinY =  1e30f, wMinZ =  1e30f;
                        float wMaxX = -1e30f, wMaxY = -1e30f, wMaxZ = -1e30f;

                        float lMinX = grp.bbox[0], lMinY = grp.bbox[1], lMinZ = grp.bbox[2];
                        float lMaxX = grp.bbox[3], lMaxY = grp.bbox[4], lMaxZ = grp.bbox[5];

                        for (int corner = 0; corner < 8; ++corner) {
                            float lx = (corner & 1) ? lMaxX : lMinX;
                            float ly = (corner & 2) ? lMaxY : lMinY;
                            float lz = (corner & 4) ? lMaxZ : lMinZ;

                            // Scale
                            float sx = lx * scale;
                            float sy2 = ly * scale;
                            float sz = lz * scale;

                            // Rotate
                            float rx = r00 * sx + r01 * sy2 + r02 * sz;
                            float ry = r10 * sx + r11 * sy2 + r12 * sz;
                            float rz = r20 * sx + r21 * sy2 + r22 * sz;

                            // Translate to world
                            float wowX = VMAP_MID - (spawn.posX + rx);
                            float wowY = VMAP_MID - (spawn.posY + ry);
                            float wowZ = spawn.posZ + rz;

                            wMinX = (std::min)(wMinX, wowX);
                            wMinY = (std::min)(wMinY, wowY);
                            wMinZ = (std::min)(wMinZ, wowZ);
                            wMaxX = (std::max)(wMaxX, wowX);
                            wMaxY = (std::max)(wMaxY, wowY);
                            wMaxZ = (std::max)(wMaxZ, wowZ);
                        }

                        range.bboxWorld[0] = wMinX;
                        range.bboxWorld[1] = wMinY;
                        range.bboxWorld[2] = wMinZ;
                        range.bboxWorld[3] = wMaxX;
                        range.bboxWorld[4] = wMaxY;
                        range.bboxWorld[5] = wMaxZ;
                        range.wmoGroupId = static_cast<uint16_t>(grp.groupWMOID);

                        groupRanges.push_back(range);
                    }
                }
                // M2 models have no groups — create a single range so they
                // participate in per-group rendering (fixes mixed-tile bug
                // where M2 geometry was in the buffer but never drawn).
                else {
                    GroupDrawRange range;
                    range.indexStart = indicesBaseBefore;
                    range.indexCount = static_cast<uint32_t>(mergedIndices.size()) - indicesBaseBefore;
                    range.mogpFlags = MOGP_EXTERIOR;  // M2s always render fully
                    range.spawnIdx = spawnCounter;
                    range.groupIdx = 0;
                    std::fill(std::begin(range.bboxWorld), std::end(range.bboxWorld), 0.0f);
                    range.wmoGroupId = 0;
                    groupRanges.push_back(range);
                }
                } // end fallback: TC collision geometry

                // Load portal data for WMO spawns (not M2)
                SpawnPortalInfo portalInfo;
                if (isWmo && !isMpqReady) {
                    static bool loggedOnce = false;
                    if (!loggedOnce) {
                        LOG(WARNING) << "[BuildingRenderer] MPQ not available for portal loading"
                                     << " (m_mpq=" << (m_mpq ? "set" : "null")
                                     << ", open=" << (m_mpq ? (m_mpq->IsOpen() ? "yes" : "no") : "n/a") << ")";
                        loggedOnce = true;
                    }
                }
                if (isMpqReady && isWmo) {
                    const WmoPortalData* pd = portalLoader.Load(spawn.modelName, *m_mpq);
                    if (pd && pd->valid) {
                        portalInfo.hasData = true;
                        portalInfo.nGroups = pd->nGroups;
                        portalInfo.portals = pd->portals;
                        portalInfo.groupNeighbors = pd->groupNeighbors;

                        // Compute rotation matrix (reuse from above if groups existed)
                        float yawRad2   = spawn.rotY * PI / 180.0f;
                        float pitchRad2 = spawn.rotX * PI / 180.0f;
                        float rollRad2  = spawn.rotZ * PI / 180.0f;

                        float cy2 = cosf(yawRad2),  sy2 = sinf(yawRad2);
                        float cp2 = cosf(pitchRad2), sp2 = sinf(pitchRad2);
                        float cr2 = cosf(rollRad2),  sr2 = sinf(rollRad2);

                        float scale2 = spawn.scale;

                        // Store 3x3 rotation*scale matrix
                        portalInfo.transform[0] = cy2 * cp2 * scale2;
                        portalInfo.transform[1] = (cy2 * sp2 * sr2 - sy2 * cr2) * scale2;
                        portalInfo.transform[2] = (cy2 * sp2 * cr2 + sy2 * sr2) * scale2;
                        portalInfo.transform[3] = sy2 * cp2 * scale2;
                        portalInfo.transform[4] = (sy2 * sp2 * sr2 + cy2 * cr2) * scale2;
                        portalInfo.transform[5] = (sy2 * sp2 * cr2 - cy2 * sr2) * scale2;
                        portalInfo.transform[6] = -sp2 * scale2;
                        portalInfo.transform[7] = cp2 * sr2 * scale2;
                        portalInfo.transform[8] = cp2 * cr2 * scale2;

                        // Store world offset
                        portalInfo.translate[0] = spawn.posX;
                        portalInfo.translate[1] = spawn.posY;
                        portalInfo.translate[2] = spawn.posZ;

                        // Transform portal vertices to world space
                        // transform[] = rotation * scale, so multiply local vertex directly
                        size_t nPortalVerts = pd->portalVertices.size() / 3;
                        portalInfo.portalVerticesWorld.resize(nPortalVerts * 3);
                        for (size_t vi = 0; vi < nPortalVerts; ++vi) {
                            float lx = pd->portalVertices[vi * 3 + 0];
                            float ly = pd->portalVertices[vi * 3 + 1];
                            float lz = pd->portalVertices[vi * 3 + 2];

                            float rx = portalInfo.transform[0] * lx
                                     + portalInfo.transform[1] * ly
                                     + portalInfo.transform[2] * lz;
                            float ry = portalInfo.transform[3] * lx
                                     + portalInfo.transform[4] * ly
                                     + portalInfo.transform[5] * lz;
                            float rz = portalInfo.transform[6] * lx
                                     + portalInfo.transform[7] * ly
                                     + portalInfo.transform[8] * lz;

                            portalInfo.portalVerticesWorld[vi * 3 + 0] = VMAP_MID - (spawn.posX + rx);
                            portalInfo.portalVerticesWorld[vi * 3 + 1] = VMAP_MID - (spawn.posY + ry);
                            portalInfo.portalVerticesWorld[vi * 3 + 2] = spawn.posZ + rz;
                        }

                    }
                }
                spawnPortals.push_back(std::move(portalInfo));

                ++spawnCounter;
                ++loadedModels;
            }

            if (!mergedVerts.empty() && !mergedIndices.empty()) {
                // Convert to GPU vertex format (normals unused — shader uses ddx/ddy)
                size_t nVerts = mergedVerts.size() / 3;
                result.indices = std::move(mergedIndices);

                result.vertices.resize(nVerts);
                for (size_t i = 0; i < nVerts; ++i) {
                    result.vertices[i].x  = mergedVerts[i * 3 + 0];
                    result.vertices[i].y  = mergedVerts[i * 3 + 1];
                    result.vertices[i].z  = mergedVerts[i * 3 + 2];
                    result.vertices[i].nx = 0.0f;
                    result.vertices[i].ny = 0.0f;
                    result.vertices[i].nz = 0.0f;
                }

                // Bake wall-culling flag: non-seed interior groups get nz = -1.0
                // so the shader can discard near-vertical faces when viewed from outside.
                for (uint16_t si = 0; si < static_cast<uint16_t>(spawnPortals.size()); ++si) {
                    const auto& sp = spawnPortals[si];
                    if (!sp.hasData) continue;
                    for (auto& r : groupRanges) {
                        if (r.spawnIdx != si) continue;
                        if (r.mogpFlags & (MOGP_EXTERIOR | MOGP_ALWAYSDRAW)) continue;
                        for (uint32_t idx = r.indexStart; idx < r.indexStart + r.indexCount; ++idx) {
                            if (idx < result.indices.size()) {
                                uint32_t vi = result.indices[idx];
                                if (vi < nVerts) result.vertices[vi].nz = -1.0f;
                            }
                        }
                    }
                }

                result.groupRanges = std::move(groupRanges);
                result.spawnPortals = std::move(spawnPortals);
                std::memcpy(result.bounds, bounds, sizeof(bounds));
                result.empty = false;

                DLOG(INFO) << "[BuildingRenderer] Tile (" << req.tileX << "," << req.tileY
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

    // VMAP geometry is collision data with inconsistent triangle winding.
    // Back-face culling would hide ~half the walls.  Use CULL_NONE so the
    // depth buffer handles occlusion instead.
    D3D11_RASTERIZER_DESC rd = {};
    rd.FillMode        = D3D11_FILL_SOLID;
    rd.CullMode        = D3D11_CULL_NONE;
    rd.FrontCounterClockwise = FALSE;
    rd.ScissorEnable   = FALSE;
    rd.DepthClipEnable = TRUE;
    if (FAILED(device->CreateRasterizerState(&rd, &m_noCullRastState))) {
        LOG(ERROR) << "[BuildingRenderer] Failed to create no-cull rasterizer state";
        Shutdown();
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
    if (m_noCullRastState) { m_noCullRastState->Release(); m_noCullRastState = nullptr; }
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
    gpu.groupRanges = result.groupRanges;
    gpu.spawnPortals = result.spawnPortals;

    m_globalMinZ = (std::min)(m_globalMinZ, result.bounds[2]);
    m_globalMaxZ = (std::max)(m_globalMaxZ, result.bounds[5]);

    m_gpuCache[{result.tileX, result.tileY}] = std::move(gpu);
    return true;
}

// ---------------------------------------------------------------------------
// Viewport update
// ---------------------------------------------------------------------------

void BuildingRenderer::UpdateViewport(uint32_t mapId, float targetX, float targetY,
                                       float cameraDistance) {
    // Flush cache when Objects toggle changes (geometry differs)
    if (m_showObjects != m_lastShowObjects) {
        m_lastShowObjects = m_showObjects;
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
        m_globalMinZ =  1e30f;
        m_globalMaxZ = -1e30f;
    }

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
// Portal culling helpers
// ---------------------------------------------------------------------------

static bool PointInAABB(float x, float y, float z, const float bb[6]) {
    return x >= bb[0] && x <= bb[3] &&
           y >= bb[1] && y <= bb[4] &&
           z >= bb[2] && z <= bb[5];
}

// Test if any vertex of a portal polygon is inside the frustum.
// Conservative: may miss portals straddling frustum edges, but fast.
static bool PortalInFrustum(const std::vector<float>& portalVertsWorld,
                             const std::vector<WmoPortal>& portals,
                             uint16_t portalIdx,
                             const float frustum[6][4]) {
    if (portalIdx >= portals.size()) return false;
    const WmoPortal& p = portals[portalIdx];
    for (uint16_t i = 0; i < p.vertexCount; ++i) {
        uint16_t vi = p.startVertex + i;
        if (vi * 3 + 2 >= portalVertsWorld.size()) continue;

        float vx = portalVertsWorld[vi * 3 + 0];
        float vy = portalVertsWorld[vi * 3 + 1];
        float vz = portalVertsWorld[vi * 3 + 2];

        bool inside = true;
        for (int pi = 0; pi < 6; ++pi) {
            float d = frustum[pi][0] * vx + frustum[pi][1] * vy +
                      frustum[pi][2] * vz + frustum[pi][3];
            if (d < 0) { inside = false; break; }
        }
        if (inside) return true;
    }
    return false;
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
    ctx->RSSetState(m_noCullRastState);
    ctx->OMSetDepthStencilState(m_pipeline.GetDSState(), 0);

    // --- Portal culling: check if camera is inside any WMO group ---
    bool cameraInsideWmo = false;
    if (m_enablePortalCulling) {
        for (const auto& [k, t] : m_gpuCache) {
            if (cameraInsideWmo) break;
            if (!t.vb || !t.ib) continue;
            for (const auto& range : t.groupRanges) {
                if (PointInAABB(camera.eyeX, camera.eyeY, camera.eyeZ,
                                range.bboxWorld)) {
                    cameraInsideWmo = true;
                    break;
                }
            }
        }
    }

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
    cb.baseColor[3] = 0.0f;  // no Z offset for buildings

    cb.heightParams[0] = m_globalMinZ;
    cb.heightParams[1] = m_globalMaxZ;
    cb.heightParams[2] = 0.0f;  // Always solid grey for buildings
    cb.heightParams[3] = 0.0f;  // Group-level portal culling; shader wall hack disabled

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

    statDrawCalls = 0;
    statVertices  = 0;

    for (const auto& [key, tile] : m_gpuCache) {
        if (!tile.vb || !tile.ib || tile.indexCount == 0) continue;
        if (!FrustumIntersectsAABB(frustum, tile)) continue;

        ctx->IASetVertexBuffers(0, 1, &tile.vb, &stride, &offset);
        ctx->IASetIndexBuffer(tile.ib, DXGI_FORMAT_R32_UINT, 0);

        if (tile.groupRanges.empty()) {
            // No group data (M2-only tiles, legacy)
            ctx->DrawIndexed(tile.indexCount, 0, 0);
            statDrawCalls++;
            statVertices += tile.indexCount;
            continue;
        }

        // --- Portal culling OFF: draw everything ---
        if (!m_enablePortalCulling) {
            for (const auto& range : tile.groupRanges) {
                ctx->DrawIndexed(range.indexCount, range.indexStart, 0);
                statDrawCalls++;
                statVertices += range.indexCount;
            }
            continue;
        }

        // --- Build per-spawn visibility via portal traversal ---
        bool hasCullingData = !tile.spawnPortals.empty();
        std::vector<std::vector<bool>> spawnVis;

        if (hasCullingData) {
            spawnVis.resize(tile.spawnPortals.size());

            for (size_t si = 0; si < tile.spawnPortals.size(); ++si) {
                const auto& pi = tile.spawnPortals[si];
                if (!pi.hasData || pi.nGroups == 0) continue;

                auto& vis = spawnVis[si];

                if (cameraInsideWmo) {
                    // --- Camera INSIDE: BFS from camera's group ---
                    int cameraGroup = -1;
                    for (const auto& range : tile.groupRanges) {
                        if (range.spawnIdx != si) continue;
                        if (range.groupIdx >= pi.nGroups) continue;
                        if (PointInAABB(camera.eyeX, camera.eyeY, camera.eyeZ,
                                        range.bboxWorld)) {
                            cameraGroup = range.groupIdx;
                            break;
                        }
                    }

                    if (cameraGroup < 0)
                        continue; // Camera not in this spawn; show all

                    vis.assign(pi.nGroups, false);
                    vis[cameraGroup] = true;

                    // Seed exterior + alwaysdraw groups
                    for (const auto& range : tile.groupRanges) {
                        if (range.spawnIdx != si) continue;
                        if (range.groupIdx >= pi.nGroups) continue;
                        if (range.mogpFlags & (MOGP_EXTERIOR | MOGP_ALWAYSDRAW))
                            vis[range.groupIdx] = true;
                    }

                    // BFS from camera group
                    struct BfsEntry { uint16_t groupIdx; int depth; };
                    std::deque<BfsEntry> bfsQ;
                    bfsQ.push_back({static_cast<uint16_t>(cameraGroup), 0});

                    while (!bfsQ.empty()) {
                        auto [g, depth] = bfsQ.front();
                        bfsQ.pop_front();
                        if (depth >= kMaxPortalBfsDepth) continue;
                        if (g >= pi.groupNeighbors.size()) continue;

                        for (const auto& nb : pi.groupNeighbors[g]) {
                            if (nb.groupIdx >= pi.nGroups) continue;
                            if (vis[nb.groupIdx]) continue;
                            if (!PortalInFrustum(pi.portalVerticesWorld,
                                                 pi.portals, nb.portalIdx, frustum))
                                continue;
                            vis[nb.groupIdx] = true;
                            bfsQ.push_back({nb.groupIdx, depth + 1});
                        }
                    }
                }
                else {
                    // --- Camera OUTSIDE: render all groups ---
                    // Leave vis empty → drawing loop treats it as "show all".
                    // Shader wall hack is disabled (heightParams.w = 0),
                    // so all faces (including interior walls) are visible.
                }
            }
        }

        // Draw groups with visibility check
        for (const auto& range : tile.groupRanges) {
            if (hasCullingData &&
                range.spawnIdx < spawnVis.size() &&
                !spawnVis[range.spawnIdx].empty()) {
                if (range.groupIdx < spawnVis[range.spawnIdx].size() &&
                    !spawnVis[range.spawnIdx][range.groupIdx])
                    continue;
            }
            ctx->DrawIndexed(range.indexCount, range.indexStart, 0);
            statDrawCalls++;
            statVertices += range.indexCount;
        }
    }

    ctx->OMSetRenderTargets(1, &rtv, nullptr);
}

} // namespace mapedit
