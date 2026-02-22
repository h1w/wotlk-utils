#pragma once
#include <cstdint>
#include <map>
#include <string>

struct ID3D11Device;

namespace mapedit {

class MpqArchiveSet;
class MapBackground;

// Parses WorldMapArea.dbc and loads world map textures from MPQ archives.
class WorldMapLoader {
public:
    // Parse WorldMapArea.dbc from the MPQ archives to build the lookup table.
    bool Initialize(MpqArchiveSet& mpq);

    // Load the continent-level world map for a given mapId.
    // Decodes BLP tiles, stitches them, and uploads to MapBackground.
    bool LoadMap(MpqArchiveSet& mpq, ID3D11Device* device,
                 uint32_t mapId, MapBackground& background);

    // Check if a world map is available for the given mapId.
    bool HasMap(uint32_t mapId) const;

private:
    struct MapEntry {
        std::string areaName;
        float locLeft  = 0; // maxY (west)
        float locRight = 0; // minY (east)
        float locTop   = 0; // minX (north)
        float locBottom= 0; // maxX (south)
    };

    // mapId -> continent-level entry
    std::map<uint32_t, MapEntry> m_entries;
};

} // namespace mapedit
