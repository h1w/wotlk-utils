#pragma once

#include <cstdint>
#include <vector>

namespace mapedit {

struct TerrainTileData;

struct TerrainVertex {
    float x, y, z;     // WoW world coordinates
    float nx, ny, nz;  // Normal vector (for lighting)
    float u, v;         // UV for texture atlas (0-1 across tile)
    float slotIndex;    // Texture array slot index (set during GPU upload)
};

struct TerrainMesh {
    int tileX = 0, tileY = 0;
    std::vector<TerrainVertex> vertices;
    std::vector<uint32_t> indices;     // triangle indices (3 per tri)
    float minX = 0, minY = 0, minZ = 0;
    float maxX = 0, maxY = 0, maxZ = 0;
};

// Generate triangle mesh from terrain tile data.
// decimation: 1=full (65K tri), 2=half (16K tri), 4=quarter (4K tri)
void GenerateTerrainMesh(const TerrainTileData& tile, int tileX, int tileY,
                          TerrainMesh& outMesh, int decimation = 1);

} // namespace mapedit
