#pragma once

#include <cstdint>
#include <string>

namespace mapedit {

struct AppSettings {
    std::string mmapDir;
    std::string wowDir;
    std::string worldGraphPath;
    std::string routesPath;
    uint32_t lastMapId = 0;
    float bgOpacity    = 0.3f;
    bool autoBgFromMpq = true;
    bool showLegend    = true;
    int  bgMode        = 1;   // MapBackgroundMode: 0=None, 1=MinimapTiles, 2=ZoneWorldMaps, 3=ADTHeightmap

    // Layer visibility
    bool showGrid    = true;
    bool showNavmesh = true;
    bool showNodes   = true;
    bool showEdges   = true;
    bool showLabels  = true;
    bool showRoutes  = true;
    bool showPath    = true;
    bool showBackground = true;
    float navmeshMinZoom = 0.15f;
    int   navmeshMaxTiles = 200;

    // 3D terrain settings
    bool showTerrain      = true;
    int  terrainColorMode = 0;    // 0=solid grey, 1=height gradient, 2=slope
    bool showBuildings    = true;

    // Window dimensions (0 = default)
    int windowWidth  = 0;
    int windowHeight = 0;

    static std::string GetSettingsPath();
    bool Load();
    bool Save() const;
};

} // namespace mapedit
