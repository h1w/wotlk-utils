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
#include "../data/terrain_loader.h"
#include "../data/terrain_mesh.h"

namespace mapedit {

struct Camera3D;

class TerrainRenderer {
public:
    bool Initialize(ID3D11Device* device, ID3D11DeviceContext* context);
    void Shutdown();

    void SetDataPath(const std::string& tcDataPath);

    // Update which terrain tiles should be loaded based on 3D camera position.
    void UpdateViewport(uint32_t mapId, float targetX, float targetY,
                        float cameraDistance);

    // Render all loaded terrain tiles.
    void Render(const Camera3D& camera, ID3D11RenderTargetView* rtv,
                ID3D11DepthStencilView* dsv);

    // Configuration
    int colorMode = 0;    // 0=solid grey, 1=height gradient, 2=slope

    // Per-frame stats (updated during Render)
    mutable int statDrawCalls = 0;
    mutable int statVertices  = 0;

private:
    struct TileGpu {
        ID3D11Buffer* vb = nullptr;
        ID3D11Buffer* ib = nullptr;
        UINT indexCount = 0;
        float minX = 0, minY = 0, minZ = 0;
        float maxX = 0, maxY = 0, maxZ = 0;
    };

    using TileKey = std::pair<int, int>;

    // Background loading types
    struct LoadRequest {
        uint32_t mapId;
        int tileX, tileY;
        int decimation;
        std::string dataPath;
    };

    struct LoadResult {
        uint32_t mapId;
        int tileX, tileY;
        std::vector<TerrainVertex> vertices;
        std::vector<uint32_t> indices;
        float minX, minY, minZ;
        float maxX, maxY, maxZ;
        bool valid;  // false = no data for this tile
    };

    // Upload a completed result to GPU
    bool UploadToGpu(const LoadResult& result);
    void ReleaseTileGpu(TileGpu& tile);
    static bool FrustumIntersectsAABB(const float planes[6][4], const TileGpu& tile);
    static int SelectLOD(float tileDist);

    // Worker thread
    void WorkerLoop();
    void StartWorker();
    void StopWorker();

    ID3D11Device*        m_device = nullptr;
    ID3D11DeviceContext* m_context = nullptr;
    TerrainPipeline      m_pipeline;

    std::map<TileKey, TileGpu> m_gpuCache;
    uint32_t m_currentMapId = UINT32_MAX;

    // Global height range across all loaded tiles (for height gradient)
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

    // Throttle GPU uploads: max N per frame
    static constexpr int kMaxUploadsPerFrame = 4;
    // Max cached terrain tiles
    static constexpr int kMaxCachedTiles = 150;
};

} // namespace mapedit
