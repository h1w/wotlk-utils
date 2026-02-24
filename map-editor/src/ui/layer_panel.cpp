#include "layer_panel.h"
#include "../render/graph_renderer.h"
#include <imgui.h>

namespace mapedit {

void LayerPanel::Render(LayerVisibility& layers, MapBackgroundMode& bgMode,
                        MinimapTileCache* minimapCache) {
    ImGui::Begin("Layers", nullptr, ImGuiWindowFlags_AlwaysAutoResize);

    ImGui::Checkbox("Background", &layers.showBackground);
    if (layers.showBackground) {
        ImGui::SameLine();
        ImGui::SetNextItemWidth(160);
        static const char* modeNames[] = {
            "None", "Minimap Tiles", "Zone World Maps (TODO)", "ADT Heightmap (TODO)"
        };
        int mode = static_cast<int>(bgMode);
        if (ImGui::Combo("##BgMode", &mode, modeNames, 4))
            bgMode = static_cast<MapBackgroundMode>(mode);

    }
    ImGui::Checkbox("Coordinate Grid", &layers.showGrid);
    ImGui::Checkbox("Navmesh", &layers.showNavmesh);
    if (layers.showNavmesh) {
        ImGui::Indent(20.0f);
        ImGui::SetNextItemWidth(120);
        ImGui::SliderFloat("Min Zoom", &layers.navmeshMinZoom, 0.01f, 2.0f, "%.2f");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Navmesh only renders when zoom >= this value.\nIncrease to reduce lag at low zoom.");
        ImGui::SetNextItemWidth(120);
        ImGui::SliderInt("Max Tiles", &layers.navmeshMaxTiles, 50, 600);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Maximum number of navmesh tiles kept in cache.\nHigher = more coverage, more memory.");
        ImGui::Unindent(20.0f);
    }
    ImGui::Checkbox("Nodes", &layers.showNodes);
    ImGui::Checkbox("Edges", &layers.showEdges);
    ImGui::Checkbox("Labels", &layers.showLabels);
    ImGui::Checkbox("Routes", &layers.showRoutes);
    ImGui::Checkbox("Path Test", &layers.showPath);

    // === 3D-specific settings ===
    ImGui::Separator();
    ImGui::Text("3D Mode");
    {
        static const char* colorModeNames[] = {
            "Flat Green", "Height Gradient", "Slope Shading", "Tile Colored"
        };
        ImGui::SetNextItemWidth(140);
        ImGui::Combo("Color Mode", &layers.navmeshColorMode, colorModeNames, 4);
        ImGui::Checkbox("Wireframe Edges", &layers.navmeshDrawEdges);
        ImGui::Checkbox("Ground Plane", &layers.showGroundPlane);
        ImGui::Checkbox("Terrain", &layers.showTerrain);
        if (layers.showTerrain) {
            ImGui::Indent(20.0f);
            static const char* terrainColorNames[] = {
                "Solid Grey", "Height Gradient", "Slope Shading"
            };
            ImGui::SetNextItemWidth(140);
            ImGui::Combo("Terrain Color", &layers.terrainColorMode, terrainColorNames, 3);
            ImGui::Checkbox("Smooth", &layers.terrainSmooth);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Push terrain slightly below navmesh to remove\nbumps where terrain pokes through.");
            ImGui::Unindent(20.0f);
        }
        ImGui::Checkbox("Buildings", &layers.showBuildings);
        if (layers.showBuildings) {
            ImGui::Indent(20.0f);
            ImGui::Checkbox("Objects", &layers.showBuildingObjects);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Show M2 collision shapes (fences, barrels, poles, etc.).\nOff by default to reduce visual noise.");
            ImGui::Checkbox("Portal Culling", &layers.enablePortalCulling);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Hide WMO interior groups not visible through portals.\nReduces visual clutter from interior walls/ceilings.");
            ImGui::Unindent(20.0f);
        }
    }

    ImGui::End();
}

} // namespace mapedit
