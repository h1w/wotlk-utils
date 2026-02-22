#include "route_renderer_3d.h"
#include "primitives_3d.h"
#include "../camera/camera3d.h"
#include "../data/route_data.h"
#include "../editor/route_editor.h"
#include <imgui.h>
#include <cmath>
#include <cstdio>

namespace mapedit {

// Elevation offset used consistently for all route geometry
static constexpr float kRouteZ = 1.0f;

void Route3DRenderer::Render(const Camera3D& camera, Primitives3D& prims,
                              const RouteData& routes, const RouteEditor& editor,
                              uint32_t mapId) {
    auto* fgDL = ImGui::GetForegroundDrawList();

    int selectedRoute = editor.GetSelectedRoute();
    int selectedWp    = editor.GetSelectedWaypoint();

    for (int ri = 0; ri < static_cast<int>(routes.GetRoutes().size()); ++ri) {
        const auto& route = routes.GetRoutes()[ri];
        if (route.mapId != mapId) continue;
        if (route.waypoints.empty()) continue;

        uint32_t color = route.color; // already ABGR
        // Boost alpha slightly for selected route to distinguish it
        if (ri == selectedRoute) {
            // replace alpha byte (bits 24-31) with 0xFF
            color = (color & 0x00FFFFFFu) | 0xFF000000u;
        }

        // Draw polyline segments
        for (size_t i = 0; i + 1 < route.waypoints.size(); ++i) {
            const auto& wp0 = route.waypoints[i];
            const auto& wp1 = route.waypoints[i + 1];
            prims.AddLine(wp0.x, wp0.y, wp0.z + kRouteZ,
                          wp1.x, wp1.y, wp1.z + kRouteZ,
                          color);
        }

        // Loop-closing line (dashed in 2D; in 3D approximate dash by
        // subdividing the segment into alternating drawn/skipped pieces)
        if (route.loop && route.waypoints.size() >= 2) {
            const auto& last  = route.waypoints.back();
            const auto& first = route.waypoints.front();

            float dx = first.x - last.x;
            float dy = first.y - last.y;
            float dz = first.z - last.z;
            float len = std::sqrt(dx * dx + dy * dy + dz * dz);

            if (len > 0.0f) {
                // Dash length in world units (approximate)
                const float dashLen = 8.0f;
                const float gapLen  = 6.0f;
                float t = 0.0f;
                while (t < len) {
                    float t0 = t / len;
                    float t1 = (std::min)((t + dashLen) / len, 1.0f);
                    prims.AddLine(
                        last.x + dx * t0, last.y + dy * t0, last.z + dz * t0 + kRouteZ,
                        last.x + dx * t1, last.y + dy * t1, last.z + dz * t1 + kRouteZ,
                        color);
                    t += dashLen + gapLen;
                }
            }
        }

        // Draw waypoint circles + number labels
        for (int i = 0; i < static_cast<int>(route.waypoints.size()); ++i) {
            const auto& wp = route.waypoints[i];
            bool isSelected = (ri == selectedRoute && i == selectedWp);
            float radius = 2.5f;

            if (isSelected) {
                // Yellow outer ring for selected waypoint
                prims.AddCircle(wp.x, wp.y, wp.z + kRouteZ,
                                radius + 2.0f, IM_COL32(255, 255, 0, 220), 20);
            }

            prims.AddCircle(wp.x, wp.y, wp.z + kRouteZ, radius, color, 12);

            // Number label projected to screen
            float sx, sy;
            if (camera.WorldToScreen(wp.x, wp.y, wp.z + kRouteZ + 1.0f, sx, sy)) {
                char buf[8];
                snprintf(buf, sizeof(buf), "%d", i + 1);
                fgDL->AddText(ImVec2(sx + 6.0f, sy - 6.0f),
                              IM_COL32(220, 220, 220, 200), buf);
            }
        }
    }
}

} // namespace mapedit
