#include "wmo_portal_loader.h"
#include "../mpq/mpq_archive.h"
#include <glog/logging.h>
#include <cstring>
#include <algorithm>
#include <cctype>

namespace mapedit {

std::string WmoPortalLoader::ResolveMpqPath(const std::string& vmapName,
                                             const MpqArchiveSet& mpq) {
    auto it = m_pathCache.find(vmapName);
    if (it != m_pathCache.end())
        return it->second;

    // Build basename index on first use: maps lowercase basename -> full MPQ path
    // for all .wmo files in the MPQ archives.
    if (m_basenameIndex.empty()) {
        LOG(INFO) << "WmoPortalLoader: building WMO basename index from MPQ...";
        auto allWmo = mpq.ListFiles("*.wmo", 50000);
        for (auto& path : allWmo) {
            // Extract basename after last backslash
            auto sep = path.rfind('\\');
            std::string bn = (sep != std::string::npos) ? path.substr(sep + 1) : path;

            // Skip WMO group files (e.g. Stormwind_000.wmo, Stormwind_001.wmo)
            // They have _NNN.wmo suffix where NNN is digits
            if (bn.size() > 8) {
                auto udot = bn.rfind('.');
                if (udot != std::string::npos && udot >= 4) {
                    auto uscore = bn.rfind('_', udot - 1);
                    if (uscore != std::string::npos && uscore + 1 < udot) {
                        bool allDigits = true;
                        for (size_t i = uscore + 1; i < udot; ++i) {
                            if (!std::isdigit(static_cast<unsigned char>(bn[i]))) {
                                allDigits = false;
                                break;
                            }
                        }
                        if (allDigits)
                            continue; // skip group file
                    }
                }
            }

            // Lowercase for case-insensitive lookup
            std::string key = bn;
            for (auto& ch : key)
                ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));

            // Only store first match per basename (highest-priority MPQ wins)
            if (m_basenameIndex.find(key) == m_basenameIndex.end())
                m_basenameIndex[key] = path;
        }
        LOG(INFO) << "WmoPortalLoader: indexed " << m_basenameIndex.size() << " root WMO files";
    }

    // Strategy 1: Try underscore-to-backslash (TC flattened worldspawn names)
    if (vmapName.find('_') != std::string::npos) {
        std::string candidate = vmapName;
        for (auto& ch : candidate) {
            if (ch == '_')
                ch = '\\';
        }
        if (mpq.HasFile(candidate)) {
            DLOG(INFO) << "WmoPortalLoader: resolved '" << vmapName << "' -> '" << candidate << "'";
            m_pathCache[vmapName] = candidate;
            return candidate;
        }
    }

    // Strategy 2: Exact basename lookup in the index
    std::string key = vmapName;
    for (auto& ch : key)
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));

    auto idxIt = m_basenameIndex.find(key);
    if (idxIt != m_basenameIndex.end()) {
        DLOG(INFO) << "WmoPortalLoader: resolved '" << vmapName << "' -> '" << idxIt->second << "'";
        m_pathCache[vmapName] = idxIt->second;
        return idxIt->second;
    }

    LOG(WARNING) << "WmoPortalLoader: could not resolve MPQ path for '" << vmapName << "'";
    m_pathCache[vmapName] = "";
    return "";
}

bool WmoPortalLoader::ParseWmoRoot(const std::vector<uint8_t>& data, WmoPortalData& out) {
    if (data.size() < 8)
        return false;

    // Log first 8 bytes for debugging chunk tag byte order
    {
        char tag0[5] = {};
        std::memcpy(tag0, data.data(), 4);
        DLOG(INFO) << "WmoPortalLoader: file size=" << data.size()
                  << ", first 4 bytes='" << tag0 << "'"
                  << " (hex: " << std::hex
                  << (int)data[0] << " " << (int)data[1] << " "
                  << (int)data[2] << " " << (int)data[3] << std::dec << ")";
    }

    uint32_t nPortals = 0;
    bool hasMohd = false;

    struct MoprEntry {
        uint16_t portalIndex;
        uint16_t groupIndex;
        int16_t  side;
        uint16_t filler;
    };
    std::vector<MoprEntry> moprEntries;

    size_t off = 0;
    while (off + 8 <= data.size()) {
        const uint8_t* ptr = data.data() + off;
        uint32_t chunkSize;
        std::memcpy(&chunkSize, ptr + 4, 4);

        if (off + 8 + chunkSize > data.size()) {
            LOG(WARNING) << "WmoPortalLoader: chunk overflows file at offset " << off;
            break;
        }

        const uint8_t* chunkData = ptr + 8;

        // MOHD: WoW WMO files store chunk tags in reversed byte order
        if (std::memcmp(ptr, "DHOM", 4) == 0 && chunkSize >= 12) {
            uint32_t nMaterials, nGroups, nPortalsLocal;
            std::memcpy(&nMaterials, chunkData + 0, 4);
            std::memcpy(&nGroups, chunkData + 4, 4);
            std::memcpy(&nPortalsLocal, chunkData + 8, 4);
            out.nGroups = nGroups;
            nPortals = nPortalsLocal;
            hasMohd = true;
        }
        // MOPV: portal vertices (reversed: 'V','P','O','M')
        else if (std::memcmp(ptr, "VPOM", 4) == 0) {
            uint32_t nVerts = chunkSize / 12;
            out.portalVertices.resize(nVerts * 3);
            std::memcpy(out.portalVertices.data(), chunkData, nVerts * 12);
        }
        // MOPT: portal definitions (reversed: 'T','P','O','M')
        else if (std::memcmp(ptr, "TPOM", 4) == 0) {
            uint32_t count = chunkSize / 20;
            out.portals.resize(count);
            for (uint32_t i = 0; i < count; ++i) {
                const uint8_t* entry = chunkData + i * 20;
                std::memcpy(&out.portals[i].startVertex, entry + 0, 2);
                std::memcpy(&out.portals[i].vertexCount, entry + 2, 2);
                std::memcpy(&out.portals[i].plane, entry + 4, 16);
            }
        }
        // MOPR: portal references (reversed: 'R','P','O','M')
        else if (std::memcmp(ptr, "RPOM", 4) == 0) {
            uint32_t count = chunkSize / 8;
            moprEntries.resize(count);
            for (uint32_t i = 0; i < count; ++i) {
                const uint8_t* entry = chunkData + i * 8;
                std::memcpy(&moprEntries[i].portalIndex, entry + 0, 2);
                std::memcpy(&moprEntries[i].groupIndex, entry + 2, 2);
                std::memcpy(&moprEntries[i].side, entry + 4, 2);
                std::memcpy(&moprEntries[i].filler, entry + 6, 2);
            }
        }

        off += 8 + chunkSize;
    }

    if (!hasMohd) {
        LOG(WARNING) << "WmoPortalLoader: MOHD chunk not found";
        return false;
    }

    // Build per-group adjacency from MOPR entries.
    // Each portal connects exactly 2 groups. Collect group indices per portal,
    // then create bidirectional neighbor links.
    std::unordered_map<uint16_t, std::vector<uint16_t>> portalGroupMap;
    for (auto& entry : moprEntries)
        portalGroupMap[entry.portalIndex].push_back(entry.groupIndex);

    out.groupNeighbors.resize(out.nGroups);
    for (auto& [portalIdx, refs] : portalGroupMap) {
        if (refs.size() == 2) {
            uint16_t a = refs[0], b = refs[1];
            if (a < out.nGroups)
                out.groupNeighbors[a].push_back({portalIdx, b});
            if (b < out.nGroups)
                out.groupNeighbors[b].push_back({portalIdx, a});
        }
    }

    out.valid = true;
    return true;
}

const WmoPortalData* WmoPortalLoader::Load(const std::string& vmapModelName,
                                            const MpqArchiveSet& mpq) {
    DLOG(INFO) << "WmoPortalLoader::Load('" << vmapModelName << "')";

    auto cacheIt = m_cache.find(vmapModelName);
    if (cacheIt != m_cache.end())
        return cacheIt->second.valid ? &cacheIt->second : nullptr;

    std::string mpqPath = ResolveMpqPath(vmapModelName, mpq);
    if (mpqPath.empty()) {
        m_cache[vmapModelName] = WmoPortalData{};
        return nullptr;
    }

    std::vector<uint8_t> fileData = mpq.ReadFile(mpqPath);
    if (fileData.empty()) {
        LOG(WARNING) << "WmoPortalLoader: failed to read '" << mpqPath << "'";
        m_cache[vmapModelName] = WmoPortalData{};
        return nullptr;
    }

    WmoPortalData portalData;
    if (!ParseWmoRoot(fileData, portalData)) {
        m_cache[vmapModelName] = WmoPortalData{};
        return nullptr;
    }

    DLOG(INFO) << "WmoPortalLoader: loaded " << portalData.portals.size()
              << " portals, " << portalData.nGroups << " groups from '" << mpqPath << "'";

    auto [it, _] = m_cache.emplace(vmapModelName, std::move(portalData));
    return &it->second;
}

void WmoPortalLoader::ClearCache() {
    m_cache.clear();
    m_pathCache.clear();
    // Keep m_basenameIndex — it's MPQ-global, doesn't change per map
}

} // namespace mapedit
