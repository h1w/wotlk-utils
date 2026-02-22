#include "path_renderer_3d.h"
#include "path_renderer.h"
#include "primitives_3d.h"
#include "../camera/camera3d.h"
#include <imgui.h>
#include <cstdio>

namespace mapedit {

// Elevation above Z=0 so path lines are visible above navmesh
static constexpr float kPathZ = 1.5f;

void Path3DRenderer::Render(const Camera3D& camera, Primitives3D& prims,
                             const PathRenderer& pathRenderer) {
    auto* fgDL = ImGui::GetForegroundDrawList();

    // Draw point A
    if (pathRenderer.HasA()) {
        float ax = pathRenderer.AX();
        float ay = pathRenderer.AY();
        float az = pathRenderer.AZ();
        // Red circle at A
        prims.AddCircle(ax, ay, az + kPathZ, 4.0f, IM_COL32(255, 50, 50, 220), 16);
        // "A" label projected to screen
        float sx, sy;
        if (camera.WorldToScreen(ax, ay, az + kPathZ + 2.0f, sx, sy)) {
            fgDL->AddText(ImVec2(sx + 8.0f, sy - 8.0f),
                          IM_COL32(255, 100, 100, 255), "A");
        }
    }

    // Draw point B
    if (pathRenderer.HasB()) {
        float bx = pathRenderer.BX();
        float by = pathRenderer.BY();
        float bz = pathRenderer.BZ();
        // Green circle at B
        prims.AddCircle(bx, by, bz + kPathZ, 4.0f, IM_COL32(50, 255, 50, 220), 16);
        // "B" label projected to screen
        float sx, sy;
        if (camera.WorldToScreen(bx, by, bz + kPathZ + 2.0f, sx, sy)) {
            fgDL->AddText(ImVec2(sx + 8.0f, sy - 8.0f),
                          IM_COL32(100, 255, 100, 255), "B");
        }
    }

    // Draw path polyline
    // Note: the 2D PathRenderer's CachedPath stores only XY (Z discarded).
    // We render at a fixed elevation (kPathZ) since actual navmesh Z is unavailable here.
    if (pathRenderer.PathValid()) {
        const auto& pts = pathRenderer.PathPts();
        if (pts.size() >= 2) {
            uint32_t pathColor = pathRenderer.PathPartial()
                                     ? IM_COL32(255, 165,   0, 220) // orange — partial
                                     : IM_COL32(255, 255,   0, 220); // yellow — complete

            for (size_t i = 0; i + 1 < pts.size(); ++i) {
                prims.AddLine(pts[i].x,   pts[i].y,   kPathZ,
                              pts[i+1].x, pts[i+1].y, kPathZ,
                              pathColor);
            }

            // Distance label at path midpoint
            size_t mid = pts.size() / 2;
            float sx, sy;
            if (camera.WorldToScreen(pts[mid].x, pts[mid].y, kPathZ + 2.0f, sx, sy)) {
                char buf[64];
                snprintf(buf, sizeof(buf), "%.0f yd%s",
                         pathRenderer.PathDistance(),
                         pathRenderer.PathPartial() ? " (partial)" : "");
                fgDL->AddText(ImVec2(sx + 5.0f, sy - 16.0f),
                              IM_COL32(255, 255, 200, 255), buf);
            }
        }
    }
}

} // namespace mapedit
