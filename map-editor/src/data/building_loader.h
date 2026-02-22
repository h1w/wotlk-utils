#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <unordered_map>

namespace mapedit {

struct BuildingMesh {
    std::string filename;
    std::vector<float> vertices;     // flat: x,y,z, x,y,z, ... (model-local coords)
    std::vector<uint32_t> indices;   // triangle indices (3 per tri)
    float bounds[6] = {};            // AABB: minX, minY, minZ, maxX, maxY, maxZ
    bool valid = false;
};

class BuildingLoader {
public:
    void SetDataPath(const std::string& tcDataPath);

    // Load a building model from Buildings/ directory.
    // Caches internally -- subsequent calls return cached data.
    const BuildingMesh* LoadBuilding(const std::string& filename);

    void ClearCache();

private:
    std::string m_dataPath;
    std::unordered_map<std::string, BuildingMesh> m_cache;

    bool ParseVmapModel(const std::string& fullPath, BuildingMesh& out);
};

} // namespace mapedit
