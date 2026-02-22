#include "grid_renderer.h"
#include "../canvas/canvas.h"
#include "../data/tile_index.h"
#include <imgui.h>
#include <cmath>

namespace mapedit {

static constexpr float kTileSize = 533.33333f;

void GridRenderer::Render(const Canvas& canvas, const TileIndex& tileIndex, uint32_t mapId) {
    RenderCoordGrid(canvas);
    RenderTileGrid(canvas, tileIndex, mapId);
}

void GridRenderer::RenderCoordGrid(const Canvas& canvas) {
    auto* dl = ImGui::GetBackgroundDrawList();

    // Adaptive grid spacing: pick spacing so grid lines are 50-200 px apart
    float pixPerYard = canvas.zoom;
    float targetSpacingPx = 100.0f;
    float spacingYards = targetSpacingPx / pixPerYard;

    // Round to a nice number (powers of 10, or 500, 1000, 5000, etc.)
    float nice[] = {10, 25, 50, 100, 250, 500, 1000, 2500, 5000, 10000, 25000};
    float gridSpacing = nice[0];
    for (float n : nice) {
        if (n >= spacingYards * 0.5f) {
            gridSpacing = n;
            break;
        }
    }

    float minX, maxX, minY, maxY;
    canvas.GetViewBounds(minX, maxX, minY, maxY);

    ImU32 gridColor = IM_COL32(60, 60, 60, 100);
    ImU32 axisColor = IM_COL32(100, 100, 100, 150);

    // Vertical lines (constant Y values -> east-west lines)
    float startY = std::floor(minY / gridSpacing) * gridSpacing;
    for (float wy = startY; wy <= maxY; wy += gridSpacing) {
        float sx1, sy1, sx2, sy2;
        canvas.WorldToScreen(minX, wy, sx1, sy1);
        canvas.WorldToScreen(maxX, wy, sx2, sy2);
        ImU32 col = (std::abs(wy) < gridSpacing * 0.01f) ? axisColor : gridColor;
        dl->AddLine(ImVec2(sx1, sy1), ImVec2(sx2, sy2), col, 1.0f);
    }

    // Horizontal lines (constant X values -> north-south lines)
    float startX = std::floor(minX / gridSpacing) * gridSpacing;
    for (float wx = startX; wx <= maxX; wx += gridSpacing) {
        float sx1, sy1, sx2, sy2;
        canvas.WorldToScreen(wx, minY, sx1, sy1);
        canvas.WorldToScreen(wx, maxY, sx2, sy2);
        ImU32 col = (std::abs(wx) < gridSpacing * 0.01f) ? axisColor : gridColor;
        dl->AddLine(ImVec2(sx1, sy1), ImVec2(sx2, sy2), col, 1.0f);
    }
}

void GridRenderer::RenderTileGrid(const Canvas& canvas, const TileIndex& tileIndex, uint32_t mapId) {
    auto* dl = ImGui::GetBackgroundDrawList();

    // Don't draw tile rects if zoomed too far out (would be invisible)
    if (canvas.zoom < 0.005f) return;

    float minX, maxX, minY, maxY;
    canvas.GetViewBounds(minX, maxX, minY, maxY);

    // Convert view bounds to tile range
    int minTileX = 31 - static_cast<int>(std::floor(maxX / kTileSize));
    int maxTileX = 31 - static_cast<int>(std::floor(minX / kTileSize));
    int minTileY = 31 - static_cast<int>(std::floor(maxY / kTileSize));
    int maxTileY = 31 - static_cast<int>(std::floor(minY / kTileSize));

    // Clamp to valid range
    minTileX = std::max(minTileX, 0);
    maxTileX = std::min(maxTileX, 63);
    minTileY = std::max(minTileY, 0);
    maxTileY = std::min(maxTileY, 63);

    ImU32 tileColor = IM_COL32(0, 120, 200, 60);
    ImU32 tileBorder = IM_COL32(0, 120, 200, 120);

    for (int tx = minTileX; tx <= maxTileX; ++tx) {
        for (int ty = minTileY; ty <= maxTileY; ++ty) {
            if (!tileIndex.HasTile(mapId, tx, ty)) continue;

            // Tile world bounds: tile tx covers wowX in [(31-tx)*TS, (32-tx)*TS)
            float wowXMin = (31 - tx) * kTileSize;
            float wowXMax = (32 - tx) * kTileSize;
            float wowYMin = (31 - ty) * kTileSize;
            float wowYMax = (32 - ty) * kTileSize;

            float sx1, sy1, sx2, sy2;
            canvas.WorldToScreen(wowXMax, wowYMax, sx1, sy1); // top-left on screen
            canvas.WorldToScreen(wowXMin, wowYMin, sx2, sy2); // bottom-right on screen

            dl->AddRectFilled(ImVec2(sx1, sy1), ImVec2(sx2, sy2), tileColor);
            dl->AddRect(ImVec2(sx1, sy1), ImVec2(sx2, sy2), tileBorder, 0.0f, 0, 1.0f);
        }
    }
}

} // namespace mapedit
