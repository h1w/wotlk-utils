#pragma once

#include <cstdint>
#include <string>

namespace mapedit {

struct AppSettings {
    std::string mmapDir;
    std::string wowDir;
    std::string worldGraphPath;
    std::string roadGraphPath;
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
    bool showWorldGraph = true;
    bool showRoadGraph = true;
    bool showRoutes  = true;
    bool showPath    = true;
    bool showBackground = true;
    float navmeshMinZoom = 0.15f;
    int   navmeshMaxTiles = 200;

    // 3D settings
    int  navmeshColorMode = 0;    // NavmeshColorMode: 0=FlatGreen, 1=HeightGradient, 2=SlopeShading, 3=TileColored
    bool navmeshDrawEdges = true;
    bool showGroundPlane  = true;
    bool showTerrain      = true;
    int  terrainColorMode = 0;    // 0=solid grey, 1=height gradient, 2=slope
    bool terrainSmooth    = false;
    bool showBuildings    = true;
    bool showBuildingObjects = false;
    bool enablePortalCulling = true;

    // Performance / view
    int  viewMode      = 0;     // 0=2D, 1=3D
    bool vsync         = false;
    int  fpsLimit      = 0;     // 0=unlimited, >0=cap
    bool showProfiler  = true;

    // Window dimensions (0 = default)
    int windowWidth  = 0;
    int windowHeight = 0;

    static std::string GetSettingsPath();
    bool Load();
    bool Save() const;
};

} // namespace mapedit
