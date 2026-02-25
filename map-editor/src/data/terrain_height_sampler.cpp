#include "terrain_height_sampler.h"
#include "world_graph_data.h"

#include <glog/logging.h>
#include <cmath>
#include <algorithm>

namespace mapedit {

void TerrainHeightSampler::SetDataPath(const std::string& tcDataPath) {
    m_loader.SetDataPath(tcDataPath);
}

void TerrainHeightSampler::ClearCache() {
    m_lru.clear();
    m_map.clear();
}

const TerrainTileData* TerrainHeightSampler::GetTile(uint32_t mapId, int tileX, int tileY) {
    CacheKey key{mapId, tileX, tileY};
    auto it = m_map.find(key);
    if (it != m_map.end()) {
        // Move to front (most recently used)
        m_lru.splice(m_lru.begin(), m_lru, it->second);
        return &it->second->data;
    }

    // Load tile
    CacheEntry entry;
    entry.key = key;
    if (!m_loader.LoadTile(mapId, tileX, tileY, entry.data))
        return nullptr;

    // Evict LRU if at capacity
    if (static_cast<int>(m_lru.size()) >= MAX_CACHED_TILES) {
        auto& back = m_lru.back();
        m_map.erase(back.key);
        m_lru.pop_back();
    }

    m_lru.push_front(std::move(entry));
    m_map[key] = m_lru.begin();
    return &m_lru.front().data;
}

float TerrainHeightSampler::InterpolateHeight(const TerrainTileData& tile, int tileX, int tileY,
                                               float wowX, float wowY) {
    // Tile origin (NW corner)
    float tileOriginX = (32 - tileX) * TILE_SIZE;
    float tileOriginY = (32 - tileY) * TILE_SIZE;

    // Local offset within tile (positive going south/east)
    float localX = tileOriginX - wowX;
    float localY = tileOriginY - wowY;

    // Cell indices
    int cellRow = static_cast<int>(std::floor(localX / CELL_SIZE));
    int cellCol = static_cast<int>(std::floor(localY / CELL_SIZE));
    cellRow = std::clamp(cellRow, 0, 127);
    cellCol = std::clamp(cellCol, 0, 127);

    // Fractional position within cell [0,1]
    float fracX = (localX - cellRow * CELL_SIZE) / CELL_SIZE;
    float fracY = (localY - cellCol * CELL_SIZE) / CELL_SIZE;
    fracX = std::clamp(fracX, 0.0f, 1.0f);
    fracY = std::clamp(fracY, 0.0f, 1.0f);

    // V9 corners (TL, TR, BL, BR) and V8 center
    float zTL = tile.v9[cellRow * 129 + cellCol];
    float zTR = tile.v9[cellRow * 129 + cellCol + 1];
    float zBL = tile.v9[(cellRow + 1) * 129 + cellCol];
    float zBR = tile.v9[(cellRow + 1) * 129 + cellCol + 1];
    float zC  = tile.v8[cellRow * 128 + cellCol];

    // Determine which triangle in the fan (center at 0.5, 0.5)
    // Diagonals split the cell into 4 triangles:
    //   top:    fracX < fracY  && fracX < (1-fracY)  ... actually use center-based fan
    // The cell is split by the two diagonals into 4 triangles meeting at center (0.5, 0.5)
    float cx = 0.5f, cy = 0.5f;

    // Which triangle? Use cross-product sign with diagonals
    // Diagonal 1: TL(0,0) to BR(1,1) → direction (1,1)
    // Diagonal 2: TR(0,1) to BL(1,0) → direction (1,-1)
    bool aboveDiag1 = (fracY - fracX) > 0;    // above TL-BR diagonal (towards TR)
    bool aboveDiag2 = (fracY + fracX) < 1.0f; // above TR-BL diagonal (towards TL)

    float z;
    if (aboveDiag1 && aboveDiag2) {
        // Top triangle: TL, TR, Center
        // Barycentric with vertices TL(0,0), TR(0,1), C(0.5,0.5)
        float area = cx * 1.0f; // = 0.5
        float w_tl = ((cx - fracX) * (1.0f - cy) - (cy - fracY) * (0.0f - cx)) / (2.0f * area);
        float w_tr = ((fracX - 0.0f) * (cy - 0.0f) - (fracY - 0.0f) * (cx - 0.0f)) / (2.0f * area);
        // Simpler: bilinear-like from the triangle
        // Actually use the standard approach: for triangle fan from center
        float u = fracX / 0.5f;       // 0 at top edge, 1 at center row
        float t = fracY;               // 0..1 along top edge
        // Lerp: top edge = lerp(zTL, zTR, t), then lerp to center
        float zTop = zTL + t * (zTR - zTL);
        z = zTop + u * (zC - zTop);
    } else if (!aboveDiag1 && !aboveDiag2) {
        // Bottom triangle: BL, BR, Center
        float u = (1.0f - fracX) / 0.5f; // 0 at bottom, 1 at center row
        float t = fracY;
        float zBot = zBL + t * (zBR - zBL);
        z = zBot + u * (zC - zBot);
    } else if (aboveDiag1 && !aboveDiag2) {
        // Right triangle: TR, BR, Center
        float u = (1.0f - fracY) / 0.5f; // 0 at right edge, 1 at center col
        float t = fracX;
        float zRight = zTR + t * (zBR - zTR);
        z = zRight + u * (zC - zRight);
    } else {
        // Left triangle: TL, BL, Center
        float u = fracY / 0.5f;          // 0 at left edge, 1 at center col
        float t = fracX;
        float zLeft = zTL + t * (zBL - zTL);
        z = zLeft + u * (zC - zLeft);
    }

    return z;
}

std::optional<float> TerrainHeightSampler::SampleHeight(uint32_t mapId, float wowX, float wowY) {
    int tileX = 31 - static_cast<int>(std::floor(wowX / TILE_SIZE));
    int tileY = 31 - static_cast<int>(std::floor(wowY / TILE_SIZE));

    const TerrainTileData* tile = GetTile(mapId, tileX, tileY);
    if (!tile)
        return std::nullopt;

    return InterpolateHeight(*tile, tileX, tileY, wowX, wowY);
}

int TerrainHeightSampler::AssignHeights(uint32_t mapId, std::vector<WorldNode>& nodes) {
    int updated = 0;
    for (auto& node : nodes) {
        if (node.mapId != mapId)
            continue;
        auto h = SampleHeight(mapId, node.x, node.y);
        if (h.has_value()) {
            node.z = h.value();
            ++updated;
        }
    }
    LOG(INFO) << "[HeightSampler] Assigned heights to " << updated << " / " << nodes.size() << " nodes";
    return updated;
}

} // namespace mapedit
