#include "terrain_loader.h"

#include <glog/logging.h>

#include <cstring>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <algorithm>

namespace fs = std::filesystem;

namespace mapedit {

// .map file header (44 bytes)
#pragma pack(push, 1)
struct MapFileHeader {
    char     mapMagic[4];        // "MAPS"
    uint32_t versionMagic;
    uint32_t buildMagic;
    uint32_t areaMapOffset;
    uint32_t areaMapSize;
    uint32_t heightMapOffset;
    uint32_t heightMapSize;
    uint32_t liquidMapOffset;
    uint32_t liquidMapSize;
    uint32_t holesOffset;
    uint32_t holesSize;
};

// Height map chunk header (16 bytes at heightMapOffset)
struct HeightChunkHeader {
    char     fourCC[4];          // "MHGT"
    uint32_t flags;
    float    gridHeight;         // base height
    float    gridMaxHeight;      // max height
};
#pragma pack(pop)

// Height flags
static constexpr uint32_t HEIGHT_NO_HEIGHT   = 0x01;
static constexpr uint32_t HEIGHT_AS_INT16    = 0x02;
static constexpr uint32_t HEIGHT_AS_INT8     = 0x04;
static constexpr uint32_t HEIGHT_HAS_FLIGHT  = 0x08;

void TerrainLoader::SetDataPath(const std::string& tcDataPath) {
    m_dataPath = tcDataPath;
}

bool TerrainLoader::LoadTile(uint32_t mapId, int tileX, int tileY, TerrainTileData& out) {
    out.valid = false;
    out.tileX = tileX;
    out.tileY = tileY;
    out.hasHoles = false;
    std::memset(out.holes, 0, sizeof(out.holes));

    if (m_dataPath.empty())
        return false;

    // Build file path: {dataPath}/maps/{mapId:03d}{tileX:02d}{tileY:02d}.map
    char filename[32];
    std::snprintf(filename, sizeof(filename), "%03u%02d%02d.map", mapId, tileX, tileY);

    fs::path filePath = fs::path(m_dataPath) / "maps" / filename;
    return ParseMapFile(filePath.string(), out);
}

bool TerrainLoader::ParseMapFile(const std::string& path, TerrainTileData& out) {
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open())
        return false;

    // Read file header
    MapFileHeader hdr;
    file.read(reinterpret_cast<char*>(&hdr), sizeof(hdr));
    if (!file || std::memcmp(hdr.mapMagic, "MAPS", 4) != 0) {
        LOG(WARNING) << "[TerrainLoader] Bad magic in " << path;
        return false;
    }

    // No height data
    if (hdr.heightMapSize == 0)
        return false;

    // Read height chunk header
    file.seekg(hdr.heightMapOffset, std::ios::beg);
    HeightChunkHeader hchdr;
    file.read(reinterpret_cast<char*>(&hchdr), sizeof(hchdr));
    if (!file || std::memcmp(hchdr.fourCC, "MHGT", 4) != 0) {
        LOG(WARNING) << "[TerrainLoader] Bad MHGT in " << path;
        return false;
    }

    // Handle NO_HEIGHT: flat tile
    if (hchdr.flags & HEIGHT_NO_HEIGHT) {
        for (int i = 0; i < 129 * 129; ++i)
            out.v9[i] = hchdr.gridHeight;
        for (int i = 0; i < 128 * 128; ++i)
            out.v8[i] = hchdr.gridHeight;
        out.minZ = out.maxZ = hchdr.gridHeight;
        out.valid = true;
        return true;
    }

    // Determine data size and read raw height bytes
    size_t v9Count = 129 * 129;
    size_t v8Count = 128 * 128;
    size_t totalValues = v9Count + v8Count;   // 33025

    size_t bytesPerValue;
    if (hchdr.flags & HEIGHT_AS_INT16)
        bytesPerValue = 2;
    else if (hchdr.flags & HEIGHT_AS_INT8)
        bytesPerValue = 1;
    else
        bytesPerValue = 4;   // float

    size_t dataBytes = totalValues * bytesPerValue;
    std::vector<uint8_t> raw(dataBytes);
    file.read(reinterpret_cast<char*>(raw.data()), dataBytes);
    if (!file) {
        LOG(WARNING) << "[TerrainLoader] Truncated height data in " << path;
        return false;
    }

    DecompressHeights(raw.data(), hchdr.flags, hchdr.gridHeight, hchdr.gridMaxHeight,
                      out.v9, out.v8);

    // Compute height range
    out.minZ =  1e30f;
    out.maxZ = -1e30f;
    for (int i = 0; i < 129 * 129; ++i) {
        out.minZ = (std::min)(out.minZ, out.v9[i]);
        out.maxZ = (std::max)(out.maxZ, out.v9[i]);
    }
    for (int i = 0; i < 128 * 128; ++i) {
        out.minZ = (std::min)(out.minZ, out.v8[i]);
        out.maxZ = (std::max)(out.maxZ, out.v8[i]);
    }

    // Read holes data
    if (hdr.holesSize > 0 && hdr.holesSize >= 512) {
        file.seekg(hdr.holesOffset, std::ios::beg);
        file.read(reinterpret_cast<char*>(out.holes), 512);
        if (file) {
            out.hasHoles = true;
            // Check if any holes are actually set
            bool anyHole = false;
            for (int i = 0; i < 256 && !anyHole; ++i)
                anyHole = (out.holes[i] != 0);
            out.hasHoles = anyHole;
        }
    }

    out.valid = true;
    return true;
}

void TerrainLoader::DecompressHeights(const uint8_t* raw, uint32_t flags,
                                       float gridH, float gridMaxH,
                                       float* v9, float* v8) {
    const size_t v9Count = 129 * 129;
    const size_t v8Count = 128 * 128;
    float range = gridMaxH - gridH;

    if (flags & HEIGHT_AS_INT16) {
        const uint16_t* data = reinterpret_cast<const uint16_t*>(raw);
        for (size_t i = 0; i < v9Count; ++i)
            v9[i] = gridH + (data[i] / 65535.0f) * range;
        for (size_t i = 0; i < v8Count; ++i)
            v8[i] = gridH + (data[v9Count + i] / 65535.0f) * range;
    } else if (flags & HEIGHT_AS_INT8) {
        for (size_t i = 0; i < v9Count; ++i)
            v9[i] = gridH + (raw[i] / 255.0f) * range;
        for (size_t i = 0; i < v8Count; ++i)
            v8[i] = gridH + (raw[v9Count + i] / 255.0f) * range;
    } else {
        // float32
        const float* data = reinterpret_cast<const float*>(raw);
        std::memcpy(v9, data, v9Count * sizeof(float));
        std::memcpy(v8, data + v9Count, v8Count * sizeof(float));
    }
}

} // namespace mapedit
