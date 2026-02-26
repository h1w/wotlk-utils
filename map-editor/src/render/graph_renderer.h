#pragma once
#include <cstdint>

namespace mapedit {

struct Canvas;
class WorldGraphData;
struct MultiSelection;

struct LayerVisibility {
    bool showNodes = true;
    bool showEdges = true;
    bool showLabels = true;
    bool showGrid = true;
    bool showNavmesh = true;
    bool showWorldGraph = true;
    bool showRoadGraph = true;
    bool showRoutes = true;
    bool showPath = true;
    bool showBackground = true;
    float navmeshMinZoom = 0.15f;
    int   navmeshMaxTiles = 200;

    // 3D-specific settings
    int  navmeshColorMode = 0;  // NavmeshColorMode enum value
    bool navmeshDrawEdges = true;
    bool showGroundPlane  = true;
    bool showTerrain      = true;
    int  terrainColorMode = 0;  // 0=solid grey, 1=height gradient, 2=slope
    bool terrainSmooth    = false;  // smooth V8 centers from V9 corner avg
    bool showTerrainTextures = false;  // overlay game textures on terrain
    bool showBuildings    = true;
    bool showBuildingObjects = false;
    bool enablePortalCulling = true;
};

class GraphRenderer {
public:
    void Render(const Canvas& canvas, const WorldGraphData& graph,
                uint32_t mapId, const MultiSelection& selection,
                const LayerVisibility& layers);

    void RenderRoadOverlay(const Canvas& canvas, const WorldGraphData& roadGraph,
                           uint32_t mapId);
};

} // namespace mapedit
