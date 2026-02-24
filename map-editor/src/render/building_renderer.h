#pragma once

#include <d3d11.h>
#include <cstdint>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "terrain_pipeline.h"
#include "../data/building_loader.h"
#include "../data/vmap_tile_loader.h"
#include "../data/wmo_portal_loader.h"
#include "../data/wmo_visual_loader.h"

namespace mapedit {

struct Camera3D;
struct WmoPortalData;
class MpqArchiveSet;

class BuildingRenderer {
public:
    bool Initialize(ID3D11Device* device, ID3D11DeviceContext* context);
    void Shutdown();

    void SetDataPath(const std::string& tcDataPath);

    // Load building geometry for visible tiles based on camera position.
    void UpdateViewport(uint32_t mapId, float targetX, float targetY,
                        float cameraDistance);

    // Render all loaded building geometry.
    void Render(const Camera3D& camera, ID3D11RenderTargetView* rtv,
                ID3D11DepthStencilView* dsv);

private:
    struct GroupDrawRange {
        uint32_t indexStart = 0;      // into tile's IB
        uint32_t indexCount = 0;
        uint32_t mogpFlags = 0;
        float    bboxWorld[6] = {};   // world-space AABB
        uint16_t spawnIdx = 0;        // which spawn in the tile
        uint16_t groupIdx = 0;        // sequential index in mesh->groups
        uint16_t wmoGroupId = 0;      // WMO group ID (for portal graph)
    };

    struct SpawnPortalInfo {
        bool hasData = false;
        uint32_t nGroups = 0;
        std::vector<float> portalVerticesWorld;  // transformed to world space
        std::vector<WmoPortal> portals;
        std::vector<std::vector<WmoPortalData::Neighbor>> groupNeighbors;
        float transform[9] = {};     // rotation*scale 3x3 matrix
        float translate[3] = {};     // world offset
    };

    struct TileBuildings {
        ID3D11Buffer* vb = nullptr;
        ID3D11Buffer* ib = nullptr;
        UINT indexCount = 0;
        float minX = 0, minY = 0, minZ = 0;
        float maxX = 0, maxY = 0, maxZ = 0;
        std::vector<GroupDrawRange> groupRanges;
        std::vector<SpawnPortalInfo> spawnPortals;
    };

    using TileKey = std::pair<int, int>;

    // Background loading types
    struct LoadRequest {
        uint32_t mapId;
        int tileX, tileY;
        std::string dataPath;
        float cameraDistance;
    };

    struct LoadResult {
        uint32_t mapId;
        int tileX, tileY;
        std::vector<TerrainVertexGpu> vertices;
        std::vector<uint32_t> indices;
        float bounds[6];
        bool empty;  // true = no vmtile or no geometry
        std::vector<GroupDrawRange> groupRanges;
        std::vector<SpawnPortalInfo> spawnPortals;
    };

    bool UploadToGpu(const LoadResult& result);
    void ReleaseTileGpu(TileBuildings& tile);
    static bool FrustumIntersectsAABB(const float planes[6][4], const TileBuildings& tile);

    // Worker thread
    void WorkerLoop();
    void StartWorker();
    void StopWorker();

    ID3D11Device*        m_device = nullptr;
    ID3D11DeviceContext* m_context = nullptr;
    TerrainPipeline      m_pipeline;
    ID3D11RasterizerState* m_noCullRastState = nullptr;

    std::map<TileKey, TileBuildings> m_gpuCache;
    uint32_t m_currentMapId = UINT32_MAX;

    // Global height range
    float m_globalMinZ =  1e30f;
    float m_globalMaxZ = -1e30f;

    // Data path (protected by mutex for worker thread)
    std::mutex m_pathMutex;
    std::string m_dataPath;

    // Worker thread
    std::thread m_worker;
    std::atomic<bool> m_running{false};

    std::mutex m_reqMutex;
    std::deque<LoadRequest> m_requests;
    std::condition_variable m_reqCV;

    std::mutex m_resMutex;
    std::deque<LoadResult> m_results;

    // Tiles currently queued for loading (main thread only)
    std::set<TileKey> m_pending;

    // Show M2 collision objects (small props like fences, barrels, etc.)
    bool m_showObjects = false;
    bool m_lastShowObjects = false;  // track for cache invalidation

    MpqArchiveSet* m_mpq = nullptr;
    bool m_enablePortalCulling = true;

    static constexpr int kMaxUploadsPerFrame = 2;
    static constexpr int kMaxCachedTiles = 100;

public:
    void SetShowObjects(bool show) { m_showObjects = show; }
    void SetMpqArchive(MpqArchiveSet* mpq) { m_mpq = mpq; }
    void SetPortalCulling(bool enable) { m_enablePortalCulling = enable; }
};

} // namespace mapedit
