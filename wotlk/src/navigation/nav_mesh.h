#pragma once
// =============================================================================
// NavMesh — loads TrinityCore .mmap/.mmtile files and manages Detour navmesh.
//
// Streams tiles around the player (5x5 grid), unloads distant tiles.
// Uses standard TrinityCore mmap format (magic 0x4D4D4150, version 16).
// =============================================================================

#include "../game/types.h"

#include <cstdint>
#include <string>
#include <map>
#include <set>
#include <utility>

struct dtNavMesh;
struct dtNavMeshQuery;
typedef unsigned int dtTileRef;

namespace nav {

class NavMesh {
public:
    static NavMesh& Instance();

    // Initialize with path to mmaps directory (e.g. "C:/mmaps/")
    bool Initialize(const char* mmapDir);
    void Shutdown();

    // Load navmesh header for a specific map (e.g. 0 = Eastern Kingdoms, 1 = Kalimdor)
    bool LoadMap(uint32_t mapId);

    // Stream tiles around position — call from EndScene (~1/sec)
    void UpdateLoadedTiles(float x, float y);

    // Access
    dtNavMesh*      GetNavMesh()  const { return m_navMesh; }
    dtNavMeshQuery* GetQuery()    const { return m_query; }
    uint32_t        GetMapId()    const { return m_mapId; }
    bool            IsReady()     const { return m_navMesh != nullptr && m_query != nullptr; }
    int             GetLoadedTileCount() const { return static_cast<int>(m_loadedTiles.size()); }
    bool            IsTileLoaded(int tileX, int tileY) const {
        return m_loadedTiles.count({tileX, tileY}) > 0;
    }

    // Load a specific set of tiles (used by CorridorLoader).
    // Returns number of newly loaded tiles.
    int LoadTiles(const std::set<std::pair<int,int>>& tiles);

    // WoW world coords -> tile coords
    static void WorldToTile(float x, float y, int& tileX, int& tileY);

private:
    NavMesh() = default;
    ~NavMesh();

    NavMesh(const NavMesh&) = delete;
    NavMesh& operator=(const NavMesh&) = delete;

    dtNavMesh*      m_navMesh = nullptr;
    dtNavMeshQuery* m_query   = nullptr;
    uint32_t        m_mapId   = 0xFFFFFFFF;
    std::string     m_mmapDir;

    // Loaded tiles: (tileX, tileY) -> dtTileRef
    std::map<std::pair<int,int>, dtTileRef> m_loadedTiles;

    // Last tile coords we streamed around (avoid reloading every frame)
    int m_lastCenterTileX = -999;
    int m_lastCenterTileY = -999;

    // Maps that failed to load — don't retry every second
    std::set<uint32_t> m_failedMaps;

    static constexpr int kTileLoadRadius   = 2;     // -2..+2 = 5x5 load grid
    static constexpr int kTileUnloadRadius = 3;     // keep tiles within 7x7, unload beyond
    static constexpr int kMaxTilesOverride = 128;   // override TC's maxTiles for 32-bit dtPolyRef (corridor + 5x5 player tiles)

    bool LoadTile(int tileX, int tileY);
    void UnloadTile(int tileX, int tileY);
    void FreeNavMesh();
};

} // namespace nav
