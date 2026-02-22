#include "grid_renderer_3d.h"
#include "primitives_3d.h"
#include "../camera/camera3d.h"
#include "../data/tile_index.h"
#include <imgui.h>
#include <cmath>

namespace mapedit {

// Tile size in WoW world units (yards)
static constexpr float kTileSize3D = 533.33333f;

// Elevation for tile boundary lines — render at Z=0 (ground plane)
static constexpr float kGridZ = 0.0f;

void Grid3DRenderer::Render(const Camera3D& camera, Primitives3D& prims,
                             const TileIndex& tileIndex, uint32_t mapId) {
    if (!tileIndex.IsScanned()) return;

    const auto& tiles = tileIndex.GetTilesForMap(mapId);
    if (tiles.empty()) return;

    // Tile boundary colour: match the 2D GridRenderer's tileBorder colour
    const uint32_t tileBorder  = IM_COL32(  0, 120, 200, 120); // blue outline
    const uint32_t tilePresent = IM_COL32(  0, 120, 200,  60); // lighter fill lines

    // Rough frustum check: get camera eye position and use distance to cull
    // tiles that are too far away to be meaningful.
    // We use the full tile set without camera-frustum plane math here since
    // the set is already small (at most 64x64 = 4096 entries).

    for (const auto& tc : tiles) {
        int tx = tc.x;
        int ty = tc.y;

        // Tile world bounds (same formula as the 2D GridRenderer):
        //   wowX ∈ [(31-tx)*TS, (32-tx)*TS)
        //   wowY ∈ [(31-ty)*TS, (32-ty)*TS)
        float xMin = (31 - tx) * kTileSize3D;
        float xMax = (32 - tx) * kTileSize3D;
        float yMin = (31 - ty) * kTileSize3D;
        float yMax = (32 - ty) * kTileSize3D;

        // Draw the 4 edges of the tile rectangle at Z=0
        //   SW->SE, SE->NE, NE->NW, NW->SW
        prims.AddLine(xMin, yMin, kGridZ,  xMax, yMin, kGridZ,  tileBorder);
        prims.AddLine(xMax, yMin, kGridZ,  xMax, yMax, kGridZ,  tileBorder);
        prims.AddLine(xMax, yMax, kGridZ,  xMin, yMax, kGridZ,  tileBorder);
        prims.AddLine(xMin, yMax, kGridZ,  xMin, yMin, kGridZ,  tileBorder);

        // Cross-hatch diagonals at lighter alpha so the tile interior is
        // distinguishable (mirrors the 2D rect fill intent)
        prims.AddLine(xMin, yMin, kGridZ,  xMax, yMax, kGridZ,  tilePresent);
        prims.AddLine(xMax, yMin, kGridZ,  xMin, yMax, kGridZ,  tilePresent);
    }

    // Tile coordinate labels projected to screen for each loaded tile
    auto* fgDL = ImGui::GetForegroundDrawList();
    for (const auto& tc : tiles) {
        int tx = tc.x;
        int ty = tc.y;

        // Centre of tile in world space
        float cx = (31 - tx) * kTileSize3D + kTileSize3D * 0.5f;
        float cy = (31 - ty) * kTileSize3D + kTileSize3D * 0.5f;

        float sx, sy;
        if (camera.WorldToScreen(cx, cy, kGridZ + 1.0f, sx, sy)) {
            char buf[32];
            snprintf(buf, sizeof(buf), "%d,%d", tx, ty);
            fgDL->AddText(ImVec2(sx - 12.0f, sy - 6.0f),
                          IM_COL32(80, 160, 220, 180), buf);
        }
    }
}

} // namespace mapedit
