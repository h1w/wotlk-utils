#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace mapedit {

class MpqArchiveSet;

struct WdtInfo {
    uint32_t mphdFlags = 0;
    bool hasBigAlpha() const { return (mphdFlags & 0x004) != 0; }
};

struct MclyEntry {
    uint32_t textureId;
    uint32_t flags;
    uint32_t offsetInMcal;
    int32_t  effectId;
};

struct AdtChunkTexture {
    int indexX = 0, indexY = 0;
    float posX = 0, posZ = 0, posY = 0;
    int nLayers = 0;
    struct Layer {
        uint32_t textureId = 0;
        uint32_t flags = 0;
        std::vector<uint8_t> alphaRaw;  // raw MCAL data for this layer
    };
    Layer layers[4];
};

struct AdtTextureData {
    bool valid = false;
    int tileX = 0, tileY = 0;
    std::vector<std::string> texturePaths;  // MTEX entries
    AdtChunkTexture chunks[16][16];         // [row][col]
};

class AdtTextureParser {
public:
    void Initialize(MpqArchiveSet* mpq);

    // Read WDT MPHD flags for a map (cached).
    WdtInfo ReadWdtMphd(const std::string& mapName);

    // Parse a single ADT tile's texture data from MPQ.
    AdtTextureData Parse(const std::string& mapName, int tileX, int tileY, uint32_t mphdFlags);

    // Resolve mapId -> internal map name (e.g. 0 -> "Azeroth") via Map.dbc.
    std::string GetMapName(uint32_t mapId);

private:
    MpqArchiveSet* m_mpq = nullptr;
    std::map<uint32_t, std::string> m_mapNames;   // mapId -> InternalName
    std::map<std::string, WdtInfo> m_wdtCache;

    void LoadMapDbc();

    // IFF chunk finder: scans from 'start' for a chunk with the given reversed tag.
    // Returns true if found, sets outOff/outSize to data offset and data size.
    static bool FindChunk(const uint8_t* data, size_t size, uint32_t tag,
                          size_t start, size_t& outOff, size_t& outSize);
};

} // namespace mapedit
