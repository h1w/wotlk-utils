#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <utility>

#include "tile_loader.h"

class dtNavMesh;
class dtNavMeshQuery;

namespace mapedit {

struct Canvas;

struct TileInfo {
    bool valid = false;
    int tileX = 0, tileY = 0;       // WoW tile coords
    int detourX = 0, detourY = 0;    // Detour tile coords
    int polyCount = 0;
    int vertCount = 0;
    int detailTriCount = 0;
    int detailVertCount = 0;
    int detailMeshCount = 0;
    int bvNodeCount = 0;
    int offMeshConCount = 0;
    int maxLinkCount = 0;
    float walkableHeight = 0;
    float walkableRadius = 0;
    float walkableClimb = 0;
    int renderTriCount = 0;    // from TileTriangles (rendering cache)
    bool inNavmesh = false;    // tile currently loaded in dtNavMesh
    bool inRenderCache = false; // has extracted rendering data
};

class TileCache {
public:
    static constexpr int kDefaultMaxTiles = 200;
    static constexpr int kMaxLoadsPerFrame = 8;    // disk I/O limit per frame
    static constexpr int kMaxNavmeshTiles  = 64;   // matches kMaxTiles in tile_loader

    ~TileCache();

    void SetMaxTiles(int max) { m_maxTiles = max; }
    int  GetMaxTiles() const  { return m_maxTiles; }

    // Initialize for a given map. Frees previous data.
    bool SetMap(const std::string& mmapDir, uint32_t mapId);

    // Update viewport: load tiles radially from center, evict distant ones.
    void UpdateViewport(const Canvas& canvas);

    // Update viewport for 3D mode: load tiles around target in a radius
    // proportional to camera distance.
    void UpdateViewport3D(float targetX, float targetY, float cameraDistance);

    // Access cached triangles for rendering
    const TileTriangles* GetTriangles(int tileX, int tileY) const;

    // Iterate all cached tiles (tileX, tileY, triangles, detourX, detourY)
    using TileCallback = std::function<void(int tileX, int tileY, const TileTriangles& tris,
                                             int detourX, int detourY)>;
    void ForEachTile(const TileCallback& cb) const;

    // Access the navmesh and query (for pathfinding)
    dtNavMesh* GetNavMesh() { return m_mesh; }
    dtNavMeshQuery* GetQuery() { return m_query; }

    uint32_t GetMapId() const { return m_mapId; }
    int GetLoadedCount() const { return static_cast<int>(m_cache.size()); }

    // Get metadata for a tile (for Properties panel)
    TileInfo GetTileInfo(int tileX, int tileY) const;

    void Clear();

private:
    // Rendering cache entry (viewport-scoped, evicted freely)
    struct CacheEntry {
        TileTriangles triangles;
        int detourX = 0;  // actual Detour tile coords (from tile header)
        int detourY = 0;
        TileInfo headerInfo;  // snapshot of dtMeshHeader at extraction time
    };

    // Tracks tiles loaded into dtNavMesh (persistent with LRU eviction)
    struct NavmeshTileEntry {
        int accessOrder = 0;
        int detourX = 0;  // actual Detour tile coords (from tile header)
        int detourY = 0;
    };

    using TileKey = std::pair<int, int>;

    dtNavMesh* m_mesh = nullptr;
    dtNavMeshQuery* m_query = nullptr;
    std::string m_mmapDir;
    uint32_t m_mapId = 0xFFFFFFFF;

    std::map<TileKey, CacheEntry> m_cache;       // rendering triangles (viewport-scoped)
    std::map<TileKey, NavmeshTileEntry> m_meshTiles; // tiles in dtNavMesh (persistent)
    std::set<TileKey> m_missingTiles;            // tiles that failed to load (no retry)
    int m_accessCounter = 0;
    int m_maxTiles = kDefaultMaxTiles;

    // Ensure tile is loaded into dtNavMesh. Returns true if tile is available.
    // Increments diskLoads if a disk read was performed.
    bool EnsureTileInNavmesh(int tileX, int tileY, int centerTX, int centerTY,
                             int& diskLoads);

    // Evict the least-recently-used tile from dtNavMesh to free a slot.
    void EvictFarthestNavmeshTile(int centerTX, int centerTY);
};

} // namespace mapedit
