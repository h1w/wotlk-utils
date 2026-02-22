#pragma once

#include <cstdint>
#include <string>

namespace mapedit {

struct TerrainTileData {
    int tileX = 0, tileY = 0;
    bool valid = false;

    // Decompressed height arrays (always float, regardless of storage format)
    float v9[129 * 129];   // outer vertices (cell corners)
    float v8[128 * 128];   // inner vertices (cell centers)

    // Holes: 16x16 grid, each uint16 is a bitmask of 4x4 sub-holes
    uint16_t holes[16 * 16];
    bool hasHoles = false;

    // Computed height range
    float minZ = 0, maxZ = 0;
};

class TerrainLoader {
public:
    void SetDataPath(const std::string& tcDataPath);

    // Load a single terrain tile. Returns false if file missing or no height data.
    bool LoadTile(uint32_t mapId, int tileX, int tileY, TerrainTileData& out);

private:
    std::string m_dataPath;   // e.g. "Z:/Games/wow 3.3.5a client/wotlk/trinitycore_data"

    bool ParseMapFile(const std::string& path, TerrainTileData& out);
    void DecompressHeights(const uint8_t* raw, uint32_t flags,
                           float gridH, float gridMaxH,
                           float* v9, float* v8);
};

} // namespace mapedit
