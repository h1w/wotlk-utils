#include "adt_texture_parser.h"
#include "../mpq/mpq_archive.h"
#include "../mpq/dbc_reader.h"

#include <glog/logging.h>

#include <cstring>
#include <sstream>

namespace mapedit {

// IFF tags are stored reversed in WoW files (e.g. MVER → bytes 'R','E','V','M').
// memcpy into uint32 on little-endian: first byte = low bits.
static constexpr uint32_t MakeTag(char c0, char c1, char c2, char c3) {
    return static_cast<uint32_t>(static_cast<uint8_t>(c0))
         | (static_cast<uint32_t>(static_cast<uint8_t>(c1)) << 8)
         | (static_cast<uint32_t>(static_cast<uint8_t>(c2)) << 16)
         | (static_cast<uint32_t>(static_cast<uint8_t>(c3)) << 24);
}
static constexpr uint32_t TAG_MVER = MakeTag('R','E','V','M');
static constexpr uint32_t TAG_MPHD = MakeTag('D','H','P','M');
static constexpr uint32_t TAG_MTEX = MakeTag('X','E','T','M');
static constexpr uint32_t TAG_MCIN = MakeTag('N','I','C','M');
static constexpr uint32_t TAG_MCNK = MakeTag('K','N','C','M');
static constexpr uint32_t TAG_MCLY = MakeTag('Y','L','C','M');
static constexpr uint32_t TAG_MCAL = MakeTag('L','A','C','M');

// ---------------------------------------------------------------------------
// IFF chunk finder
// ---------------------------------------------------------------------------

bool AdtTextureParser::FindChunk(const uint8_t* data, size_t size, uint32_t tag,
                                  size_t start, size_t& outOff, size_t& outSize) {
    size_t pos = start;
    while (pos + 8 <= size) {
        uint32_t chunkTag;
        std::memcpy(&chunkTag, data + pos, 4);
        uint32_t chunkSize;
        std::memcpy(&chunkSize, data + pos + 4, 4);
        if (chunkTag == tag) {
            outOff = pos + 8;
            outSize = chunkSize;
            return true;
        }
        pos += 8 + chunkSize;
    }
    return false;
}

// ---------------------------------------------------------------------------
// Map.dbc loading
// ---------------------------------------------------------------------------

void AdtTextureParser::LoadMapDbc() {
    if (!m_mpq || !m_mapNames.empty()) return;

    auto data = m_mpq->ReadFile("DBFilesClient\\Map.dbc");
    if (data.empty()) {
        LOG(WARNING) << "[AdtTextureParser] Map.dbc not found in MPQ";
        return;
    }

    DbcReader dbc;
    if (!dbc.Load(data)) {
        LOG(WARNING) << "[AdtTextureParser] Failed to parse Map.dbc";
        return;
    }

    for (uint32_t i = 0; i < dbc.GetRecordCount(); ++i) {
        uint32_t mapId = dbc.GetUInt(i, 0);
        const char* name = dbc.GetString(i, 1);
        if (name && name[0])
            m_mapNames[mapId] = name;
    }

    LOG(INFO) << "[AdtTextureParser] Loaded " << m_mapNames.size() << " map names from Map.dbc";
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

void AdtTextureParser::Initialize(MpqArchiveSet* mpq) {
    m_mpq = mpq;
}

std::string AdtTextureParser::GetMapName(uint32_t mapId) {
    LoadMapDbc();
    auto it = m_mapNames.find(mapId);
    if (it != m_mapNames.end())
        return it->second;
    return {};
}

// ---------------------------------------------------------------------------
// WDT MPHD reader
// ---------------------------------------------------------------------------

WdtInfo AdtTextureParser::ReadWdtMphd(const std::string& mapName) {
    auto it = m_wdtCache.find(mapName);
    if (it != m_wdtCache.end())
        return it->second;

    WdtInfo info;

    std::string path = "World\\Maps\\" + mapName + "\\" + mapName + ".wdt";
    auto data = m_mpq->ReadFile(path);
    if (data.empty()) {
        LOG(WARNING) << "[AdtTextureParser] WDT not found: " << path;
        m_wdtCache[mapName] = info;
        return info;
    }

    // Find MPHD chunk
    size_t off, sz;
    if (FindChunk(data.data(), data.size(), TAG_MPHD, 0, off, sz) && sz >= 4) {
        std::memcpy(&info.mphdFlags, data.data() + off, 4);
    }

    m_wdtCache[mapName] = info;
    LOG(INFO) << "[AdtTextureParser] WDT " << mapName << " MPHD flags=0x"
              << std::hex << info.mphdFlags << std::dec;
    return info;
}

// ---------------------------------------------------------------------------
// MTEX parser
// ---------------------------------------------------------------------------

static std::vector<std::string> ParseMtex(const uint8_t* data, size_t size) {
    std::vector<std::string> paths;
    size_t start = 0;
    for (size_t i = 0; i < size; ++i) {
        if (data[i] == 0) {
            if (i > start)
                paths.emplace_back(reinterpret_cast<const char*>(data + start), i - start);
            start = i + 1;
        }
    }
    return paths;
}

// ---------------------------------------------------------------------------
// ADT parser
// ---------------------------------------------------------------------------

AdtTextureData AdtTextureParser::Parse(const std::string& mapName, int tileX, int tileY,
                                        uint32_t mphdFlags) {
    AdtTextureData result;
    result.tileX = tileX;
    result.tileY = tileY;

    if (!m_mpq) return result;

    // ADT path: World\Maps\{Name}\{Name}_{tileY}_{tileX}.adt
    // (A=tileY maps to wowY axis, B=tileX maps to wowX axis)
    std::ostringstream ss;
    ss << "World\\Maps\\" << mapName << "\\" << mapName << "_" << tileY << "_" << tileX << ".adt";
    std::string path = ss.str();

    auto fileData = m_mpq->ReadFile(path);
    if (fileData.empty())
        return result;

    const uint8_t* raw = fileData.data();
    size_t rawSize = fileData.size();

    // Verify MVER
    size_t off, sz;
    if (!FindChunk(raw, rawSize, TAG_MVER, 0, off, sz) || sz < 4)
        return result;
    uint32_t version;
    std::memcpy(&version, raw + off, 4);
    if (version != 18)
        return result;

    // Parse MTEX
    if (!FindChunk(raw, rawSize, TAG_MTEX, 0, off, sz))
        return result;
    result.texturePaths = ParseMtex(raw + off, sz);

    // Parse MCIN (256 entries x 16 bytes, absolute offsets)
    size_t mcinOff, mcinSz;
    if (!FindChunk(raw, rawSize, TAG_MCIN, 0, mcinOff, mcinSz))
        return result;

    for (int i = 0; i < 256; ++i) {
        size_t entryPos = mcinOff + i * 16;
        if (entryPos + 16 > rawSize) break;

        uint32_t mcnkAbsOffset;
        std::memcpy(&mcnkAbsOffset, raw + entryPos, 4);
        if (mcnkAbsOffset == 0) continue;
        if (mcnkAbsOffset + 128 > rawSize) continue;

        // MCNK header starts 8 bytes after tag
        size_t hdrPos = mcnkAbsOffset + 8;
        if (hdrPos + 128 > rawSize) continue;

        uint32_t indexX, indexY, nLayers;
        std::memcpy(&indexX, raw + hdrPos + 0x04, 4);
        std::memcpy(&indexY, raw + hdrPos + 0x08, 4);
        std::memcpy(&nLayers, raw + hdrPos + 0x0C, 4);

        if (indexX >= 16 || indexY >= 16) continue;
        if (nLayers == 0 || nLayers > 4) {
            // Valid to have 0 layers (no texture), but nothing to parse
            if (nLayers > 4) continue;
        }

        uint32_t ofsMcly, ofsMcal, sizeAlpha;
        std::memcpy(&ofsMcly, raw + hdrPos + 0x1C, 4);
        std::memcpy(&ofsMcal, raw + hdrPos + 0x24, 4);
        std::memcpy(&sizeAlpha, raw + hdrPos + 0x28, 4);

        // Position at 0x68: stored as (wowX, wowZ, wowY)
        float posX, posZ, posY;
        std::memcpy(&posX, raw + hdrPos + 0x68, 4);
        std::memcpy(&posZ, raw + hdrPos + 0x6C, 4);
        std::memcpy(&posY, raw + hdrPos + 0x70, 4);

        auto& chunk = result.chunks[indexY][indexX];
        chunk.indexX = static_cast<int>(indexX);
        chunk.indexY = static_cast<int>(indexY);
        chunk.posX = posX;
        chunk.posZ = posZ;
        chunk.posY = posY;
        chunk.nLayers = static_cast<int>(nLayers);

        // Parse MCLY sub-chunk
        if (nLayers > 0 && ofsMcly > 0) {
            size_t mclyTagPos = mcnkAbsOffset + ofsMcly;
            size_t mclyDataPos = 0, mclySz = 0;

            // Try to find the MCLY tag at the expected position
            if (mclyTagPos + 8 <= rawSize) {
                uint32_t foundTag;
                std::memcpy(&foundTag, raw + mclyTagPos, 4);
                if (foundTag == TAG_MCLY) {
                    std::memcpy(&mclySz, raw + mclyTagPos + 4, 4);
                    mclyDataPos = mclyTagPos + 8;
                } else {
                    // Fallback: data starts after assumed tag+size
                    mclyDataPos = mclyTagPos + 8;
                    mclySz = nLayers * 16;
                }
            }

            for (uint32_t li = 0; li < nLayers && li < 4; ++li) {
                size_t entryOff = mclyDataPos + li * 16;
                if (entryOff + 16 > rawSize) break;

                uint32_t texId, flags, offsetInMcal;
                int32_t effectId;
                std::memcpy(&texId, raw + entryOff, 4);
                std::memcpy(&flags, raw + entryOff + 4, 4);
                std::memcpy(&offsetInMcal, raw + entryOff + 8, 4);
                std::memcpy(&effectId, raw + entryOff + 12, 4);

                chunk.layers[li].textureId = texId;
                chunk.layers[li].flags = flags;
            }

            // Extract MCAL raw data per layer
            if (sizeAlpha > 0 && ofsMcal > 0) {
                size_t mcalTagPos = mcnkAbsOffset + ofsMcal;
                size_t mcalDataPos = 0;
                uint32_t mcalChunkSize = sizeAlpha;

                if (mcalTagPos + 8 <= rawSize) {
                    uint32_t foundTag;
                    std::memcpy(&foundTag, raw + mcalTagPos, 4);
                    if (foundTag == TAG_MCAL) {
                        std::memcpy(&mcalChunkSize, raw + mcalTagPos + 4, 4);
                        mcalDataPos = mcalTagPos + 8;
                    } else {
                        mcalDataPos = mcalTagPos + 8;
                    }
                }

                // For each layer beyond 0 that has alpha, extract raw bytes
                for (uint32_t li = 1; li < nLayers && li < 4; ++li) {
                    MclyEntry mcly;
                    size_t entryOff = mclyDataPos + li * 16;
                    if (entryOff + 16 > rawSize) break;
                    std::memcpy(&mcly.textureId, raw + entryOff, 4);
                    std::memcpy(&mcly.flags, raw + entryOff + 4, 4);
                    std::memcpy(&mcly.offsetInMcal, raw + entryOff + 8, 4);
                    std::memcpy(&mcly.effectId, raw + entryOff + 12, 4);

                    if (!(mcly.flags & 0x100))
                        continue;  // No alpha map for this layer

                    size_t alphaStart = mcalDataPos + mcly.offsetInMcal;

                    // Determine end offset
                    size_t alphaEnd;
                    if (li + 1 < nLayers) {
                        uint32_t nextOffset;
                        size_t nextEntryOff = mclyDataPos + (li + 1) * 16;
                        if (nextEntryOff + 12 <= rawSize) {
                            std::memcpy(&nextOffset, raw + nextEntryOff + 8, 4);
                            alphaEnd = mcalDataPos + nextOffset;
                        } else {
                            alphaEnd = mcalDataPos + sizeAlpha;
                        }
                    } else {
                        alphaEnd = mcalDataPos + sizeAlpha;
                    }

                    if (alphaStart < rawSize && alphaEnd <= rawSize && alphaEnd > alphaStart) {
                        chunk.layers[li].alphaRaw.assign(raw + alphaStart, raw + alphaEnd);
                    }
                }
            }
        }
    }

    result.valid = true;
    DLOG(INFO) << "[AdtTextureParser] Parsed " << mapName << "_" << tileY << "_" << tileX
              << ": " << result.texturePaths.size() << " textures";
    return result;
}

} // namespace mapedit
