#include "vmap_tile_loader.h"

#include <glog/logging.h>
#include <filesystem>
#include <fstream>
#include <cstring>
#include <cstdio>

namespace fs = std::filesystem;

namespace mapedit {

// Assembled VMAP magic: "VMAP_4.8" (8 bytes)
static constexpr char kVmapMagic[8] = {'V','M','A','P','_','4','.','8'};

static constexpr uint32_t MOD_HAS_BOUND = 0x04;

void VMapTileLoader::SetDataPath(const std::string& tcDataPath) {
    m_dataPath = tcDataPath;
}

bool VMapTileLoader::LoadTile(uint32_t mapId, int tileX, int tileY, VMapTileData& out) {
    out.tileX = tileX;
    out.tileY = tileY;
    out.valid = false;

    // vmtile naming: mapID_tileY_tileX.vmtile (Y before X!)
    char filename[64];
    snprintf(filename, sizeof(filename), "%03u_%02d_%02d.vmtile", mapId, tileY, tileX);

    std::string fullPath = (fs::path(m_dataPath) / "vmaps" / filename).string();

    if (!ParseVmtile(fullPath, out))
        return false;

    out.valid = true;
    return true;
}

bool VMapTileLoader::ParseVmtile(const std::string& path, VMapTileData& out) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file.is_open())
        return false;

    size_t fileSize = static_cast<size_t>(file.tellg());
    if (fileSize < 12)  // magic(8) + nSpawns(4)
        return false;

    std::vector<uint8_t> data(fileSize);
    file.seekg(0);
    file.read(reinterpret_cast<char*>(data.data()), fileSize);

    const uint8_t* buf = data.data();
    size_t off = 0;

    // Verify magic
    if (std::memcmp(buf, kVmapMagic, 8) != 0) {
        LOG(WARNING) << "[VMapTileLoader] Bad magic in " << path;
        return false;
    }
    off = 8;

    // Read nSpawns
    uint32_t nSpawns = 0;
    std::memcpy(&nSpawns, buf + off, 4); off += 4;

    if (nSpawns == 0)
        return false;

    if (nSpawns > 50000) {
        LOG(WARNING) << "[VMapTileLoader] Suspicious nSpawns=" << nSpawns << " in " << path;
        return false;
    }

    out.spawns.reserve(nSpawns);

    for (uint32_t i = 0; i < nSpawns; ++i) {
        ModelSpawn spawn;

        // flags (uint32)
        if (off + 4 > fileSize) break;
        std::memcpy(&spawn.flags, buf + off, 4); off += 4;

        // adtId (uint16)
        if (off + 2 > fileSize) break;
        std::memcpy(&spawn.adtId, buf + off, 2); off += 2;

        // ID (uint32)
        if (off + 4 > fileSize) break;
        std::memcpy(&spawn.id, buf + off, 4); off += 4;

        // iPos (3 floats)
        if (off + 12 > fileSize) break;
        std::memcpy(&spawn.posX, buf + off, 4); off += 4;
        std::memcpy(&spawn.posY, buf + off, 4); off += 4;
        std::memcpy(&spawn.posZ, buf + off, 4); off += 4;

        // iRot (3 floats, degrees)
        if (off + 12 > fileSize) break;
        std::memcpy(&spawn.rotX, buf + off, 4); off += 4;
        std::memcpy(&spawn.rotY, buf + off, 4); off += 4;
        std::memcpy(&spawn.rotZ, buf + off, 4); off += 4;

        // iScale (float)
        if (off + 4 > fileSize) break;
        std::memcpy(&spawn.scale, buf + off, 4); off += 4;

        // Bounds (only if MOD_HAS_BOUND)
        if (spawn.flags & MOD_HAS_BOUND) {
            if (off + 24 > fileSize) break;
            std::memcpy(spawn.boundsLow, buf + off, 12); off += 12;
            std::memcpy(spawn.boundsHigh, buf + off, 12); off += 12;
        }

        // nameLen (uint32) + name (nameLen chars, NOT null-terminated)
        if (off + 4 > fileSize) break;
        uint32_t nameLen = 0;
        std::memcpy(&nameLen, buf + off, 4); off += 4;

        if (nameLen == 0 || nameLen > 1024 || off + nameLen > fileSize) {
            LOG(WARNING) << "[VMapTileLoader] Bad nameLen=" << nameLen
                         << " at spawn " << i << " in " << path;
            break;
        }

        spawn.modelName.assign(reinterpret_cast<const char*>(buf + off), nameLen);
        off += nameLen;

        // nodeIdx (uint32) — BIH tree index, skip
        if (off + 4 > fileSize) break;
        off += 4;

        out.spawns.push_back(std::move(spawn));
    }

    if (out.spawns.empty())
        return false;

    return true;
}

} // namespace mapedit
