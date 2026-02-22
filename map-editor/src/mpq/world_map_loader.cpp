#include "world_map_loader.h"
#include "mpq_archive.h"
#include "dbc_reader.h"
#include "blp_decoder.h"
#include "../render/map_background.h"

#include <glog/logging.h>
#include <cstring>
#include <d3d11.h>

namespace mapedit {

bool WorldMapLoader::Initialize(MpqArchiveSet& mpq) {
    m_entries.clear();

    auto dbcData = mpq.ReadFile("DBFilesClient\\WorldMapArea.dbc");
    if (dbcData.empty()) {
        LOG(ERROR) << "[WorldMapLoader] WorldMapArea.dbc not found in MPQ archives";
        return false;
    }

    DbcReader dbc;
    if (!dbc.Load(dbcData)) {
        LOG(ERROR) << "[WorldMapLoader] Failed to parse WorldMapArea.dbc";
        return false;
    }

    // WorldMapArea.dbc fields (3.3.5a):
    //  0: ID
    //  1: MapID
    //  2: AreaID (0 = continent level)
    //  3: AreaName (string)
    //  4: LocLeft  (float, west  = maxY)
    //  5: LocRight (float, east  = minY)
    //  6: LocTop   (float, north = minX)
    //  7: LocBottom(float, south = maxX)
    //  8: DisplayMapID (-1 for default)
    //  9: DefaultDungeonFloor
    // 10: ParentWorldMapID

    for (uint32_t i = 0; i < dbc.GetRecordCount(); i++) {
        uint32_t mapId  = dbc.GetUInt(i, 1);
        uint32_t areaId = dbc.GetUInt(i, 2);

        // Prefer continent-level entries (AreaID == 0).
        // Only overwrite if we don't have one yet, or this is the continent level.
        auto it = m_entries.find(mapId);
        if (it != m_entries.end() && areaId != 0)
            continue; // already have a continent-level entry

        const char* areaName = dbc.GetString(i, 3);
        float locLeft   = dbc.GetFloat(i, 4);
        float locRight  = dbc.GetFloat(i, 5);
        float locTop    = dbc.GetFloat(i, 6);
        float locBottom = dbc.GetFloat(i, 7);

        // Skip entries with no name or zero-sized bounds
        if (!areaName || areaName[0] == '\0') continue;
        if (locLeft == 0 && locRight == 0 && locTop == 0 && locBottom == 0) continue;

        MapEntry entry;
        entry.areaName = areaName;
        entry.locLeft   = locLeft;
        entry.locRight  = locRight;
        entry.locTop    = locTop;
        entry.locBottom = locBottom;
        m_entries[mapId] = entry;
    }

    LOG(INFO) << "[WorldMapLoader] Loaded " << m_entries.size()
              << " world map entries from DBC";
    return !m_entries.empty();
}

bool WorldMapLoader::HasMap(uint32_t mapId) const {
    return m_entries.count(mapId) > 0;
}

bool WorldMapLoader::LoadMap(MpqArchiveSet& mpq, ID3D11Device* device,
                              uint32_t mapId, MapBackground& background) {
    auto it = m_entries.find(mapId);
    if (it == m_entries.end()) return false;

    const MapEntry& entry = it->second;

    // World map textures are stored as 4 columns x 3 rows of tiles.
    // Path: Interface\WorldMap\{AreaName}\{AreaName}{1-12}.blp
    // Tile numbering: 1-4 = top row (left to right), 5-8 = middle, 9-12 = bottom.
    constexpr int kCols = 4;
    constexpr int kRows = 3;

    // Load and decode all tiles
    BlpImage tiles[kRows * kCols];
    int tileW = 0, tileH = 0;
    bool anyLoaded = false;

    for (int row = 0; row < kRows; row++) {
        for (int col = 0; col < kCols; col++) {
            int tileNum = row * kCols + col + 1;
            char path[256];
            snprintf(path, sizeof(path), "Interface\\WorldMap\\%s\\%s%d.blp",
                     entry.areaName.c_str(), entry.areaName.c_str(), tileNum);

            auto blpData = mpq.ReadFile(path);
            if (blpData.empty()) continue;

            int idx = row * kCols + col;
            if (!DecodeBlp(blpData.data(), blpData.size(), tiles[idx])) {
                LOG(WARNING) << "[WorldMapLoader] Failed to decode: " << path;
                continue;
            }

            if (tileW == 0) {
                tileW = tiles[idx].width;
                tileH = tiles[idx].height;
            }
            anyLoaded = true;
        }
    }

    if (!anyLoaded || tileW == 0 || tileH == 0) {
        LOG(WARNING) << "[WorldMapLoader] No tiles found for map "
                     << mapId << " (" << entry.areaName << ")";
        return false;
    }

    // Stitch tiles into one image
    int totalW = tileW * kCols;
    int totalH = tileH * kRows;
    std::vector<uint8_t> composite(totalW * totalH * 4, 0);

    for (int row = 0; row < kRows; row++) {
        for (int col = 0; col < kCols; col++) {
            int idx = row * kCols + col;
            const BlpImage& tile = tiles[idx];
            if (tile.bgra.empty()) continue;

            int tw = tile.width;
            int th = tile.height;
            for (int y = 0; y < th && (row * tileH + y) < totalH; y++) {
                int dstOffset = ((row * tileH + y) * totalW + col * tileW) * 4;
                int srcOffset = y * tw * 4;
                int copyLen = tw * 4;
                if (col * tileW + tw > totalW)
                    copyLen = (totalW - col * tileW) * 4;
                std::memcpy(composite.data() + dstOffset,
                            tile.bgra.data() + srcOffset, copyLen);
            }
        }
    }

    // Upload to MapBackground
    // DBC coordinates: LocLeft=maxY(west), LocRight=minY(east),
    //                  LocTop=minX(north), LocBottom=maxX(south)
    float minX = entry.locTop;
    float maxX = entry.locBottom;
    float minY = entry.locRight;
    float maxY = entry.locLeft;

    bool ok = background.LoadFromPixels(device, composite.data(),
                                         totalW, totalH,
                                         minX, maxX, minY, maxY);
    if (ok) {
        LOG(INFO) << "[WorldMapLoader] Loaded map " << mapId
                  << " (" << entry.areaName << ") " << totalW << "x" << totalH
                  << " bounds=[" << minX << "," << maxX << "]x["
                  << minY << "," << maxY << "]";
    }
    return ok;
}

} // namespace mapedit
