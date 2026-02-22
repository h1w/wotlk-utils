#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

struct ID3D11Device;
struct ID3D11ShaderResourceView;

namespace mapedit {

struct Canvas;
class MpqArchiveSet;

enum class MapBackgroundMode { None, MinimapTiles, ZoneWorldMaps, ADTHeightmap };

class MinimapTileCache {
public:
    ~MinimapTileCache() { Shutdown(); }

    // Parse Map.dbc -> mapId->InternalName, start worker thread.
    bool Initialize(MpqArchiveSet& mpq);
    void Shutdown();

    void SetMap(uint32_t mapId);
    void UpdateViewport(const Canvas& canvas, MpqArchiveSet& mpq,
                        ID3D11Device* device);
    void Render(const Canvas& canvas);

    bool HasMap(uint32_t mapId) const;
    bool IsLoading() const { return !m_pending.empty(); }

    float GetOpacity() const { return m_opacity; }
    void  SetOpacity(float a) { m_opacity = a; }

    // Iterate over all loaded tiles (for 3D ground plane rendering).
    // Callback receives tile coords and the SRV for each cached tile.
    void ForEachCachedTile(
        const std::function<void(int tx, int ty,
                                 ID3D11ShaderResourceView* srv)>& callback) const;

private:
    // Map.dbc lookup
    std::map<uint32_t, std::string> m_mapNames; // mapId -> InternalName

    // md5translate.trs: "MapName\mapXX_YY" -> "Textures\Minimap\{hash}.blp"
    std::map<std::string, std::string> m_tilePathLookup;

    // Current state
    uint32_t m_currentMapId = 0xFFFFFFFF;
    std::string m_currentMapName;
    float m_opacity = 0.3f;

    // GPU texture cache (permanent — all tiles for current map)
    using TileKey = std::pair<int, int>;
    struct CachedTile {
        ID3D11ShaderResourceView* srv = nullptr;
    };
    std::map<TileKey, CachedTile> m_cache;
    std::set<TileKey> m_pending;      // tiles currently being decoded
    std::set<TileKey> m_missingTiles; // tiles confirmed not in MPQ
    bool m_allLoaded = false;         // true once all tiles queued for current map

    // Worker thread
    std::thread m_worker;
    std::atomic<bool> m_running{false};

    struct DecodeRequest {
        int tileX, tileY;
        std::vector<uint8_t> blpData;
    };
    struct DecodeResult {
        int tileX, tileY;
        std::vector<uint8_t> bgra;
        int width, height;
    };

    std::mutex m_requestMutex;
    std::deque<DecodeRequest> m_requests;
    std::condition_variable m_requestCV;

    std::mutex m_resultMutex;
    std::deque<DecodeResult> m_results;

    void WorkerLoop();
    void ProcessCompletedTiles(ID3D11Device* device);
    void ClearCache();
};

} // namespace mapedit
