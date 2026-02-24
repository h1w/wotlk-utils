#include "wmo_visual_loader.h"
#include "../mpq/mpq_archive.h"
#include <glog/logging.h>
#include <cstring>
#include <algorithm>
#include <cctype>
#include <cstdio>

namespace mapedit {

// ---------------------------------------------------------------------------
// MPQ path resolution (same 2-tier strategy as WmoPortalLoader)
// ---------------------------------------------------------------------------

std::string WmoVisualLoader::ResolveMpqPath(const std::string& vmapName,
                                             const MpqArchiveSet& mpq) {
    auto it = m_pathCache.find(vmapName);
    if (it != m_pathCache.end())
        return it->second;

    // Build basename index on first use
    if (m_basenameIndex.empty()) {
        LOG(INFO) << "WmoVisualLoader: building WMO basename index from MPQ...";
        auto allWmo = mpq.ListFiles("*.wmo", 50000);
        for (auto& path : allWmo) {
            auto sep = path.rfind('\\');
            std::string bn = (sep != std::string::npos) ? path.substr(sep + 1) : path;

            // Skip WMO group files (_NNN.wmo)
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
                            continue;
                    }
                }
            }

            std::string key = bn;
            for (auto& ch : key)
                ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));

            if (m_basenameIndex.find(key) == m_basenameIndex.end())
                m_basenameIndex[key] = path;
        }
        LOG(INFO) << "WmoVisualLoader: indexed " << m_basenameIndex.size() << " root WMO files";
    }

    // Strategy 1: underscore-to-backslash
    if (vmapName.find('_') != std::string::npos) {
        std::string candidate = vmapName;
        for (auto& ch : candidate) {
            if (ch == '_')
                ch = '\\';
        }
        if (mpq.HasFile(candidate)) {
            m_pathCache[vmapName] = candidate;
            return candidate;
        }
    }

    // Strategy 2: basename lookup
    std::string key = vmapName;
    for (auto& ch : key)
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));

    auto idxIt = m_basenameIndex.find(key);
    if (idxIt != m_basenameIndex.end()) {
        m_pathCache[vmapName] = idxIt->second;
        return idxIt->second;
    }

    m_pathCache[vmapName] = "";
    return "";
}

// ---------------------------------------------------------------------------
// Parse root WMO for group count (MOHD chunk -> nGroups at offset +4)
// ---------------------------------------------------------------------------

uint32_t WmoVisualLoader::ParseRootForGroupCount(const std::vector<uint8_t>& data) {
    size_t off = 0;
    while (off + 8 <= data.size()) {
        const uint8_t* ptr = data.data() + off;
        uint32_t chunkSize;
        std::memcpy(&chunkSize, ptr + 4, 4);

        if (off + 8 + chunkSize > data.size())
            break;

        // MOHD: reversed tag "DHOM"
        if (std::memcmp(ptr, "DHOM", 4) == 0 && chunkSize >= 8) {
            uint32_t nGroups;
            std::memcpy(&nGroups, ptr + 8 + 4, 4);  // offset +4 within chunk data
            return nGroups;
        }

        off += 8 + chunkSize;
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Parse WMO group file -> WmoGroupVisual
// ---------------------------------------------------------------------------

bool WmoVisualLoader::ParseGroupFile(const std::vector<uint8_t>& data, WmoGroupVisual& out) {
    if (data.size() < 8)
        return false;

    size_t off = 0;

    // Skip optional MVER chunk
    if (data.size() >= 8 && std::memcmp(data.data(), "REVM", 4) == 0) {
        uint32_t mverSize;
        std::memcpy(&mverSize, data.data() + 4, 4);
        off = 8 + mverSize;
    }

    // Read MOGP wrapping chunk
    if (off + 8 > data.size() || std::memcmp(data.data() + off, "PGOM", 4) != 0) {
        LOG(WARNING) << "WmoVisualLoader: MOGP chunk not found";
        return false;
    }

    uint32_t mogpSize;
    std::memcpy(&mogpSize, data.data() + off + 4, 4);
    size_t mogpBodyStart = off + 8;
    size_t mogpBodyEnd = mogpBodyStart + mogpSize;

    if (mogpBodyEnd > data.size()) {
        LOG(WARNING) << "WmoVisualLoader: MOGP chunk overflows file";
        return false;
    }

    if (mogpSize < 68) {
        LOG(WARNING) << "WmoVisualLoader: MOGP header too small (" << mogpSize << " bytes)";
        return false;
    }

    const uint8_t* mogpHeader = data.data() + mogpBodyStart;

    // mogpFlags at offset 0x08
    std::memcpy(&out.mogpFlags, mogpHeader + 0x08, 4);

    // Bounding box at offset 0x0C (6 floats: minXYZ, maxXYZ in file coords)
    float fileBBox[6];
    std::memcpy(fileBBox, mogpHeader + 0x0C, 24);

    // No coord swap — TC extractor writes MOVT/bbox as-is from WMO file
    std::memcpy(out.bbox, fileBBox, sizeof(fileBBox));

    // Parse sub-chunks within MOGP body (after 68-byte header)
    size_t subOff = mogpBodyStart + 68;

    struct MopyEntry { uint8_t flags; uint8_t materialId; };
    std::vector<MopyEntry> mopyEntries;
    std::vector<uint16_t> rawIndices;

    while (subOff + 8 <= mogpBodyEnd) {
        const uint8_t* ptr = data.data() + subOff;
        uint32_t chunkSize;
        std::memcpy(&chunkSize, ptr + 4, 4);

        if (subOff + 8 + chunkSize > mogpBodyEnd)
            break;

        const uint8_t* chunkData = ptr + 8;

        // MOPY: per-triangle flags + materialId (2 bytes each)
        if (std::memcmp(ptr, "YPOM", 4) == 0) {
            uint32_t nTris = chunkSize / 2;
            mopyEntries.resize(nTris);
            for (uint32_t i = 0; i < nTris; ++i) {
                mopyEntries[i].flags = chunkData[i * 2 + 0];
                mopyEntries[i].materialId = chunkData[i * 2 + 1];
            }
        }
        // MOVI: uint16 triangle indices
        else if (std::memcmp(ptr, "IVOM", 4) == 0) {
            uint32_t nIdx = chunkSize / 2;
            rawIndices.resize(nIdx);
            for (uint32_t i = 0; i < nIdx; ++i)
                std::memcpy(&rawIndices[i], chunkData + i * 2, 2);
        }
        // MOVT: float3 vertex positions
        else if (std::memcmp(ptr, "TVOM", 4) == 0) {
            uint32_t nVerts = chunkSize / 12;
            out.positions.resize(nVerts * 3);
            for (uint32_t i = 0; i < nVerts; ++i) {
                float fx, fy, fz;
                std::memcpy(&fx, chunkData + i * 12 + 0, 4);
                std::memcpy(&fy, chunkData + i * 12 + 4, 4);
                std::memcpy(&fz, chunkData + i * 12 + 8, 4);
                // No coord swap — TC extractor writes MOVT as-is from WMO file
                out.positions[i * 3 + 0] = fx;
                out.positions[i * 3 + 1] = fy;
                out.positions[i * 3 + 2] = fz;
            }
        }
        // MONR: float3 normals
        else if (std::memcmp(ptr, "RNOM", 4) == 0) {
            uint32_t nNorms = chunkSize / 12;
            out.normals.resize(nNorms * 3);
            for (uint32_t i = 0; i < nNorms; ++i) {
                float nx, ny, nz;
                std::memcpy(&nx, chunkData + i * 12 + 0, 4);
                std::memcpy(&ny, chunkData + i * 12 + 4, 4);
                std::memcpy(&nz, chunkData + i * 12 + 8, 4);
                out.normals[i * 3 + 0] = nx;
                out.normals[i * 3 + 1] = ny;
                out.normals[i * 3 + 2] = nz;
            }
        }
        // MOTV: float2 texcoords (Phase 2)
        else if (std::memcmp(ptr, "VTOM", 4) == 0) {
            out.texcoords.resize(chunkSize / 4);
            std::memcpy(out.texcoords.data(), chunkData, chunkSize);
        }
        // MOBA: 24-byte render batches (Phase 2)
        else if (std::memcmp(ptr, "ABOM", 4) == 0) {
            out.batchData.assign(chunkData, chunkData + chunkSize);
        }

        subOff += 8 + chunkSize;
    }

    // Apply MOPY filtering: skip collision-only triangles (materialId == 0xFF)
    if (!mopyEntries.empty()) {
        uint32_t nTris = static_cast<uint32_t>(mopyEntries.size());
        for (uint32_t i = 0; i < nTris && i * 3 + 2 < rawIndices.size(); ++i) {
            if (mopyEntries[i].materialId == 0xFF)
                continue;
            out.indices.push_back(static_cast<uint32_t>(rawIndices[i * 3 + 0]));
            out.indices.push_back(static_cast<uint32_t>(rawIndices[i * 3 + 1]));
            out.indices.push_back(static_cast<uint32_t>(rawIndices[i * 3 + 2]));
        }
    } else {
        // No MOPY chunk: include all triangles
        out.indices.reserve(rawIndices.size());
        for (uint16_t idx : rawIndices)
            out.indices.push_back(static_cast<uint32_t>(idx));
    }

    return !out.positions.empty() && !out.indices.empty();
}

// ---------------------------------------------------------------------------
// Load: resolve path -> parse root -> load groups -> cache
// ---------------------------------------------------------------------------

const WmoVisualData* WmoVisualLoader::Load(const std::string& vmapModelName,
                                            const MpqArchiveSet& mpq) {
    auto cacheIt = m_cache.find(vmapModelName);
    if (cacheIt != m_cache.end())
        return cacheIt->second.valid ? &cacheIt->second : nullptr;

    std::string mpqPath = ResolveMpqPath(vmapModelName, mpq);
    if (mpqPath.empty()) {
        m_cache[vmapModelName] = WmoVisualData{};
        return nullptr;
    }

    std::vector<uint8_t> rootData = mpq.ReadFile(mpqPath);
    if (rootData.empty()) {
        LOG(WARNING) << "WmoVisualLoader: failed to read root '" << mpqPath << "'";
        m_cache[vmapModelName] = WmoVisualData{};
        return nullptr;
    }

    uint32_t nGroups = ParseRootForGroupCount(rootData);
    if (nGroups == 0 || nGroups > 10000) {
        LOG(WARNING) << "WmoVisualLoader: invalid nGroups=" << nGroups
                     << " for '" << mpqPath << "'";
        m_cache[vmapModelName] = WmoVisualData{};
        return nullptr;
    }

    // Build group file paths: {rootStem}_{NNN}.wmo
    std::string rootStem = mpqPath.substr(0, mpqPath.size() - 4);

    WmoVisualData result;
    result.nGroups = nGroups;

    uint32_t totalVerts = 0, totalTris = 0;
    for (uint32_t g = 0; g < nGroups; ++g) {
        char suffix[16];
        snprintf(suffix, sizeof(suffix), "_%03u.wmo", g);
        std::string groupPath = rootStem + suffix;

        std::vector<uint8_t> groupData = mpq.ReadFile(groupPath);
        if (groupData.empty()) {
            result.groups.emplace_back();  // empty placeholder (keeps indices aligned)
            continue;
        }

        WmoGroupVisual group;
        if (ParseGroupFile(groupData, group)) {
            totalVerts += static_cast<uint32_t>(group.positions.size() / 3);
            totalTris += static_cast<uint32_t>(group.indices.size() / 3);
            result.groups.push_back(std::move(group));
        } else {
            result.groups.emplace_back();
        }
    }

    result.valid = true;
    DLOG(INFO) << "WmoVisualLoader: loaded '" << mpqPath << "' — "
              << nGroups << " groups, " << totalVerts << " verts, " << totalTris << " tris";

    auto [it, _] = m_cache.emplace(vmapModelName, std::move(result));
    return &it->second;
}

void WmoVisualLoader::ClearCache() {
    m_cache.clear();
    m_pathCache.clear();
    // Keep m_basenameIndex — it's MPQ-global, doesn't change per map
}

} // namespace mapedit
