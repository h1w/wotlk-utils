#pragma once
#include <cstdint>
#include <set>
#include <string>
#include <vector>
#include <map>

namespace mapedit {

struct TileCoord {
    int x, y;
    bool operator<(const TileCoord& o) const {
        return x < o.x || (x == o.x && y < o.y);
    }
};

class TileIndex {
public:
    // Scan a directory for .mmtile files and index which tiles exist per map
    void ScanDirectory(const std::string& dir);

    // Check if a tile exists for a given map
    bool HasTile(uint32_t mapId, int tileX, int tileY) const;

    // Get all tile coords for a map
    const std::set<TileCoord>& GetTilesForMap(uint32_t mapId) const;

    // Get all map IDs that have tiles
    std::vector<uint32_t> GetMapIds() const;

    const std::string& GetDirectory() const { return m_dir; }
    bool IsScanned() const { return m_scanned; }

private:
    std::string m_dir;
    bool m_scanned = false;
    std::map<uint32_t, std::set<TileCoord>> m_tiles;
    static const std::set<TileCoord> s_empty;
};

} // namespace mapedit
