#include "building_loader.h"

#include <glog/logging.h>
#include <filesystem>
#include <fstream>
#include <cstring>
#include <algorithm>

namespace fs = std::filesystem;

namespace mapedit {

// TC raw extractor magic: "VMAP048\0" (8 bytes)
static constexpr char kRawMagic[8] = {'V','M','A','P','0','4','8','\0'};

void BuildingLoader::SetDataPath(const std::string& tcDataPath) {
    m_dataPath = tcDataPath;
}

const BuildingMesh* BuildingLoader::LoadBuilding(const std::string& filename) {
    // Check cache first
    auto it = m_cache.find(filename);
    if (it != m_cache.end())
        return it->second.valid ? &it->second : nullptr;

    // Build full path: {dataPath}/Buildings/{filename}
    std::string fullPath = (fs::path(m_dataPath) / "Buildings" / filename).string();

    BuildingMesh mesh;
    mesh.filename = filename;

    if (!ParseVmapModel(fullPath, mesh)) {
        mesh.valid = false;
        m_cache[filename] = std::move(mesh);
        return nullptr;
    }

    mesh.valid = true;
    auto [insertIt, _] = m_cache.emplace(filename, std::move(mesh));
    return &insertIt->second;
}

void BuildingLoader::ClearCache() {
    m_cache.clear();
}

// Read a uint32 from a buffer at offset, advance offset.
static bool ReadU32(const uint8_t* data, size_t size, size_t& off, uint32_t& out) {
    if (off + 4 > size) return false;
    std::memcpy(&out, data + off, 4);
    off += 4;
    return true;
}

static bool ReadU16(const uint8_t* data, size_t size, size_t& off, uint16_t& out) {
    if (off + 2 > size) return false;
    std::memcpy(&out, data + off, 2);
    off += 2;
    return true;
}

static bool ReadFloat(const uint8_t* data, size_t size, size_t& off, float& out) {
    if (off + 4 > size) return false;
    std::memcpy(&out, data + off, 4);
    off += 4;
    return true;
}

static bool MatchTag(const uint8_t* data, size_t size, size_t off, const char* tag) {
    if (off + 4 > size) return false;
    return std::memcmp(data + off, tag, 4) == 0;
}

bool BuildingLoader::ParseVmapModel(const std::string& fullPath, BuildingMesh& out) {
    std::ifstream file(fullPath, std::ios::binary | std::ios::ate);
    if (!file.is_open())
        return false;

    size_t fileSize = static_cast<size_t>(file.tellg());
    if (fileSize < 20)
        return false;

    std::vector<uint8_t> data(fileSize);
    file.seekg(0);
    file.read(reinterpret_cast<char*>(data.data()), fileSize);

    const uint8_t* buf = data.data();
    size_t off = 0;

    // Verify magic
    if (fileSize < 8 || std::memcmp(buf, kRawMagic, 8) != 0) {
        LOG(WARNING) << "[BuildingLoader] Bad magic in " << fullPath;
        return false;
    }
    off = 8;

    // Root header: nVertices (total), nGroups, rootWMOID
    uint32_t totalVerts = 0, nGroups = 0, rootWMOID = 0;
    if (!ReadU32(buf, fileSize, off, totalVerts)) return false;
    if (!ReadU32(buf, fileSize, off, nGroups)) return false;
    if (!ReadU32(buf, fileSize, off, rootWMOID)) return false;

    if (nGroups == 0 || nGroups > 10000) {
        LOG(WARNING) << "[BuildingLoader] Invalid nGroups=" << nGroups << " in " << fullPath;
        return false;
    }

    out.rootWMOID = rootWMOID;

    // Initialize bounds
    float bMin[3] = { 1e30f,  1e30f,  1e30f};
    float bMax[3] = {-1e30f, -1e30f, -1e30f};

    out.groups.reserve(nGroups);

    // Parse each group
    for (uint32_t g = 0; g < nGroups; ++g) {
        // Per-group header: mogpFlags(4) + groupWMOID(4) + bbox(24) + liquidType(4) = 36 bytes
        if (off + 36 > fileSize) {
            LOG(WARNING) << "[BuildingLoader] Truncated group header at group " << g
                         << " in " << fullPath;
            break;
        }

        BuildingGroup group;
        uint32_t liquidType = 0;
        ReadU32(buf, fileSize, off, group.mogpFlags);
        ReadU32(buf, fileSize, off, group.groupWMOID);
        for (int i = 0; i < 6; ++i)
            ReadFloat(buf, fileSize, off, group.bbox[i]);
        ReadU32(buf, fileSize, off, liquidType);

        // Record offsets before geometry
        group.indexOffset = static_cast<uint32_t>(out.indices.size());
        group.vertexOffset = static_cast<uint32_t>(out.vertices.size() / 3);

        // "GRP " chunk — skip it
        if (!MatchTag(buf, fileSize, off, "GRP ")) {
            LOG(WARNING) << "[BuildingLoader] Expected GRP at offset " << off
                         << " in " << fullPath;
            break;
        }
        off += 4;  // skip tag
        uint32_t grpSize = 0;
        if (!ReadU32(buf, fileSize, off, grpSize)) break;
        off += grpSize;  // skip GRP data

        // "INDX" chunk — triangle indices (uint16)
        if (!MatchTag(buf, fileSize, off, "INDX")) {
            LOG(WARNING) << "[BuildingLoader] Expected INDX at offset " << off
                         << " in " << fullPath;
            break;
        }
        off += 4;  // skip tag
        uint32_t indxSize = 0, nIndices = 0;
        if (!ReadU32(buf, fileSize, off, indxSize)) break;
        if (!ReadU32(buf, fileSize, off, nIndices)) break;

        if (nIndices == 0 || off + nIndices * 2 > fileSize) {
            off += nIndices * 2;  // advance past index data
        } else {
            // Read uint16 indices, offset by current vertex base
            uint32_t vertexBase = static_cast<uint32_t>(out.vertices.size() / 3);
            out.indices.reserve(out.indices.size() + nIndices);
            for (uint32_t i = 0; i < nIndices; ++i) {
                uint16_t idx = 0;
                ReadU16(buf, fileSize, off, idx);
                out.indices.push_back(vertexBase + idx);
            }
        }

        // "VERT" chunk — vertex positions (float3)
        if (!MatchTag(buf, fileSize, off, "VERT")) {
            LOG(WARNING) << "[BuildingLoader] Expected VERT at offset " << off
                         << " in " << fullPath;
            break;
        }
        off += 4;  // skip tag
        uint32_t vertSize = 0, nVerts = 0;
        if (!ReadU32(buf, fileSize, off, vertSize)) break;
        if (!ReadU32(buf, fileSize, off, nVerts)) break;

        if (nVerts == 0 || off + nVerts * 12 > fileSize) {
            off += nVerts * 12;  // advance past vertex data
        } else {
            out.vertices.reserve(out.vertices.size() + nVerts * 3);
            for (uint32_t v = 0; v < nVerts; ++v) {
                float x, y, z;
                ReadFloat(buf, fileSize, off, x);
                ReadFloat(buf, fileSize, off, y);
                ReadFloat(buf, fileSize, off, z);
                out.vertices.push_back(x);
                out.vertices.push_back(y);
                out.vertices.push_back(z);

                // Update bounds
                bMin[0] = (std::min)(bMin[0], x);
                bMin[1] = (std::min)(bMin[1], y);
                bMin[2] = (std::min)(bMin[2], z);
                bMax[0] = (std::max)(bMax[0], x);
                bMax[1] = (std::max)(bMax[1], y);
                bMax[2] = (std::max)(bMax[2], z);
            }
        }

        // "LIQU" chunk — skip if present
        if (off < fileSize && MatchTag(buf, fileSize, off, "LIQU")) {
            off += 4;  // skip tag
            uint32_t liquSize = 0;
            if (!ReadU32(buf, fileSize, off, liquSize)) break;
            off += liquSize;  // skip liquid data
        }

        // Record counts after geometry
        group.indexCount = static_cast<uint32_t>(out.indices.size()) - group.indexOffset;
        group.vertexCount = static_cast<uint32_t>(out.vertices.size() / 3) - group.vertexOffset;
        out.groups.push_back(group);
    }

    if (out.vertices.empty() || out.indices.empty())
        return false;

    // Store bounds
    for (int i = 0; i < 3; ++i) {
        out.bounds[i] = bMin[i];
        out.bounds[i + 3] = bMax[i];
    }

    return true;
}

} // namespace mapedit
