#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <unordered_map>

namespace mapedit {

class MpqArchiveSet;

struct WmoGroupVisual {
    std::vector<float> positions;     // flat x,y,z after coord swap (WMO file -> TC local)
    std::vector<float> normals;       // flat nx,ny,nz after coord swap (Phase 2)
    std::vector<uint32_t> indices;    // filtered triangle indices (post-MOPY)
    uint32_t mogpFlags = 0;
    float bbox[6] = {};               // model-local AABB (coord-swapped): min xyz, max xyz
    // Phase 2 stubs
    std::vector<float> texcoords;     // flat u,v per vertex
    std::vector<uint8_t> batchData;   // raw MOBA entries
};

struct WmoVisualData {
    bool valid = false;
    uint32_t nGroups = 0;
    std::vector<WmoGroupVisual> groups;
};

class WmoVisualLoader {
public:
    const WmoVisualData* Load(const std::string& vmapModelName,
                               const MpqArchiveSet& mpq);
    void ClearCache();
private:
    std::unordered_map<std::string, WmoVisualData> m_cache;
    std::unordered_map<std::string, std::string> m_pathCache;
    std::unordered_map<std::string, std::string> m_basenameIndex;

    std::string ResolveMpqPath(const std::string& vmapName, const MpqArchiveSet& mpq);
    uint32_t ParseRootForGroupCount(const std::vector<uint8_t>& data);
    bool ParseGroupFile(const std::vector<uint8_t>& data, WmoGroupVisual& out);
};

} // namespace mapedit
