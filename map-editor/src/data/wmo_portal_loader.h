#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <unordered_map>

namespace mapedit {

class MpqArchiveSet;

struct WmoPortal {
    uint16_t startVertex;
    uint16_t vertexCount;
    float    plane[4];      // normal(xyz) + distance
};

struct WmoPortalData {
    bool valid = false;
    uint32_t nGroups = 0;
    std::vector<float> portalVertices;           // MOPV: flat x,y,z
    std::vector<WmoPortal> portals;              // MOPT
    // Per-group adjacency: group G -> list of {portalIdx, connectedGroupIdx}
    struct Neighbor { uint16_t portalIdx; uint16_t groupIdx; };
    std::vector<std::vector<Neighbor>> groupNeighbors;
};

class WmoPortalLoader {
public:
    const WmoPortalData* Load(const std::string& vmapModelName,
                               const MpqArchiveSet& mpq);
    void ClearCache();
private:
    std::unordered_map<std::string, WmoPortalData> m_cache;
    std::unordered_map<std::string, std::string> m_pathCache;
    std::unordered_map<std::string, std::string> m_basenameIndex; // lowercase basename -> full MPQ path
    std::string ResolveMpqPath(const std::string& vmapName, const MpqArchiveSet& mpq);
    bool ParseWmoRoot(const std::vector<uint8_t>& data, WmoPortalData& out);
};

} // namespace mapedit
