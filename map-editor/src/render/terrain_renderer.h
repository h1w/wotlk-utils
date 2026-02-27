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
#include "terrain_texture_pipeline.h"
#include "../data/terrain_loader.h"
#include "../data/terrain_mesh.h"
#include "../data/bc1_compressor.h"

namespace mapedit {

struct Camera3D;
class MpqArchiveSet;

class TerrainRenderer {
public:
    bool Initialize(ID3D11Device* device, ID3D11DeviceContext* context);
    void Shutdown();

    void SetDataPath(const std::string& tcDataPath);

    // MPQ archive for ADT texture loading (set from App before UpdateViewport)
    void SetMpqArchive(MpqArchiveSet* mpq);
    void SetMapName(const std::string& name);
    void SetTexturesEnabled(bool enabled);

    // Update which terrain tiles should be loaded based on 3D camera position.
    void UpdateViewport(uint32_t mapId, float targetX, float targetY,
                        float cameraDistance);

    // Render all loaded terrain tiles.
    void Render(const Camera3D& camera, ID3D11RenderTargetView* rtv,
                ID3D11DepthStencilView* dsv);

    // Configuration
    int colorMode = 0;    // 0=solid grey, 1=height gradient, 2=slope
    bool smoothTerrain = false;  // smooth V8 from V9 corner averages

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
        int decimation = 0;   // LOD level this tile was loaded at
        int textureSlot = -1; // index into texture array (-1 = no texture)
        bool hasTexture = false;
    };

    // Shared Texture2DArray for all tile textures
    struct TextureAtlasArray {
        ID3D11Texture2D*          texture = nullptr;
        ID3D11ShaderResourceView* srv = nullptr;
        uint32_t capacity = 0;
        uint32_t mipCount = 0;
        std::vector<int> freeSlots;

        int AllocateSlot() {
            if (freeSlots.empty()) return -1;
            int slot = freeSlots.back();
            freeSlots.pop_back();
            return slot;
        }
        void FreeSlot(int slot) {
            if (slot >= 0)
                freeSlots.push_back(slot);
        }
        void Release() {
            if (srv)     { srv->Release();     srv = nullptr; }
            if (texture) { texture->Release(); texture = nullptr; }
            freeSlots.clear();
            capacity = 0;
        }
    };

    using TileKey = std::pair<int, int>;

    // Background loading types
    struct LoadRequest {
        uint32_t mapId;
        int tileX, tileY;
        int decimation;
        std::string dataPath;
        // Texture loading context
        bool loadTextures = false;
        std::string mapName;
    };

    struct LoadResult {
        uint32_t mapId;
        int tileX, tileY;
        int decimation = 0;
        std::vector<TerrainVertex> vertices;
        std::vector<uint32_t> indices;
        float minX, minY, minZ;
        float maxX, maxY, maxZ;
        bool valid;  // false = no data for this tile
        // BC1-compressed texture atlas with mip chain
        CompressedAtlas compressedAtlas;
        bool hasTexture = false;
    };

    // Upload a completed result to GPU (non-const: patches vertex slotIndex)
    bool UploadToGpu(LoadResult& result);
    void ReleaseTileGpu(TileGpu& tile);
    static bool FrustumIntersectsAABB(const float planes[6][4], const TileGpu& tile);
    static int SelectLOD(float tileDist, float cameraDist);

    // Worker thread
    void WorkerLoop();
    void StartWorker();
    void StopWorker();

    ID3D11Device*        m_device = nullptr;
    ID3D11DeviceContext* m_context = nullptr;
    TerrainPipeline           m_pipeline;       // procedural (buildings share this)
    TerrainTexturePipeline    m_texPipeline;     // textured terrain

    std::map<TileKey, TileGpu> m_gpuCache;
    TextureAtlasArray m_texArray;
    uint32_t m_currentMapId = UINT32_MAX;

    // Global height range across all loaded tiles (for height gradient)
    float m_globalMinZ =  1e30f;
    float m_globalMaxZ = -1e30f;

    // Data path (protected by mutex for worker thread)
    std::mutex m_pathMutex;
    std::string m_dataPath;

    // Texture loading config (protected by m_texMutex — held only briefly)
    std::mutex m_texMutex;
    MpqArchiveSet* m_mpq = nullptr;
    std::string m_mapName;
    bool m_texturesEnabled = false;

    // Worker threads (parallel: mesh loading + BC1 compress, serialized: MPQ reads)
    static constexpr int kWorkerCount = 3;
    std::vector<std::thread> m_workers;
    std::atomic<bool> m_running{false};
    std::mutex m_mpqMutex;  // serializes MPQ reads across workers

    std::mutex m_reqMutex;
    std::deque<LoadRequest> m_requests;
    std::condition_variable m_reqCV;

    std::mutex m_resMutex;
    std::deque<LoadResult> m_results;

    // Tiles currently queued for loading (main thread only).
    // Value = decimation level being loaded.
    std::map<TileKey, int> m_pending;

    // Throttle GPU uploads: max N per frame
    static constexpr int kMaxUploadsPerFrame = 4;
    // Max cached terrain tiles
    static constexpr int kMaxCachedTiles = 150;
};

} // namespace mapedit
