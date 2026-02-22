#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace mapedit {

struct ModelSpawn {
    uint32_t flags = 0;        // MOD_M2=1, MOD_WORLDSPAWN=2, MOD_HAS_BOUND=4
    uint16_t adtId = 0;
    uint32_t id = 0;
    float posX = 0, posY = 0, posZ = 0;   // internal VMAP coordinates
    float rotX = 0, rotY = 0, rotZ = 0;   // Euler degrees (x=pitch, y=yaw, z=roll)
    float scale = 1.0f;
    float boundsLow[3] = {};               // only if flags & MOD_HAS_BOUND
    float boundsHigh[3] = {};
    std::string modelName;                 // filename in Buildings/
};

struct VMapTileData {
    int tileX = 0, tileY = 0;
    std::vector<ModelSpawn> spawns;
    bool valid = false;
};

class VMapTileLoader {
public:
    void SetDataPath(const std::string& tcDataPath);

    // Load spawn placements for a map tile.
    // Note: vmtile naming is mapID_tileY_tileX.vmtile (Y before X).
    bool LoadTile(uint32_t mapId, int tileX, int tileY, VMapTileData& out);

private:
    std::string m_dataPath;
    bool ParseVmtile(const std::string& path, VMapTileData& out);
};

} // namespace mapedit
