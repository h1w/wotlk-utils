#pragma once

#include "terrain_loader.h"
#include <cstdint>
#include <list>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace mapedit {

struct WorldNode;

class TerrainHeightSampler {
public:
    void SetDataPath(const std::string& tcDataPath);

    // Sample terrain height at a WoW coordinate. Returns nullopt if tile not available.
    std::optional<float> SampleHeight(uint32_t mapId, float wowX, float wowY);

    // Assign terrain heights to all nodes (batch). Returns count of updated nodes.
    int AssignHeights(uint32_t mapId, std::vector<WorldNode>& nodes);

    void ClearCache();

private:
    static constexpr int MAX_CACHED_TILES = 64;
    static constexpr float TILE_SIZE = 533.33333f;
    static constexpr float CELL_SIZE = TILE_SIZE / 128.0f;

    struct CacheKey {
        uint32_t mapId;
        int tileX, tileY;
        bool operator==(const CacheKey& o) const {
            return mapId == o.mapId && tileX == o.tileX && tileY == o.tileY;
        }
    };
    struct CacheKeyHash {
        size_t operator()(const CacheKey& k) const {
            return std::hash<uint64_t>()(
                (static_cast<uint64_t>(k.mapId) << 32) |
                (static_cast<uint32_t>(k.tileX) << 16) |
                static_cast<uint16_t>(k.tileY));
        }
    };

    struct CacheEntry {
        CacheKey key;
        TerrainTileData data;
    };

    // LRU: front = most recent
    std::list<CacheEntry> m_lru;
    std::unordered_map<CacheKey, std::list<CacheEntry>::iterator, CacheKeyHash> m_map;

    TerrainLoader m_loader;

    const TerrainTileData* GetTile(uint32_t mapId, int tileX, int tileY);
    float InterpolateHeight(const TerrainTileData& tile, int tileX, int tileY,
                            float wowX, float wowY);
};

} // namespace mapedit
