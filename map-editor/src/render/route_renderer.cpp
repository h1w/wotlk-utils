#include "route_renderer.h"
#include "../canvas/canvas.h"
#include "../data/route_data.h"
#include "../editor/route_editor.h"
#include <imgui.h>
#include <cmath>
#include <cstdio>

namespace mapedit {

static ImDrawList* BeginOverlay(const char* name, const Canvas& canvas) {
    ImGui::SetNextWindowPos(ImVec2(canvas.vpX, canvas.vpY));
    ImGui::SetNextWindowSize(ImVec2(canvas.vpW, canvas.vpH));
    ImGui::SetNextWindowBgAlpha(0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::Begin(name, nullptr,
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
        ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoInputs);
    ImGui::PopStyleVar(2);
    return ImGui::GetWindowDrawList();
}

void RouteRenderer::Render(const Canvas& canvas, const RouteData& data,
                           const RouteEditor& editor, uint32_t mapId) {
    auto* dl = BeginOverlay("##RouteOverlay", canvas);

    int selectedRoute = editor.GetSelectedRoute();
    int selectedWp = editor.GetSelectedWaypoint();

    for (int ri = 0; ri < static_cast<int>(data.GetRoutes().size()); ++ri) {
        const auto& route = data.GetRoutes()[ri];
        if (route.mapId != mapId) continue;
        if (route.waypoints.empty()) continue;

        ImU32 color = route.color;
        float thickness = (ri == selectedRoute) ? 2.5f : 1.5f;

        // Draw polyline
        for (size_t i = 0; i + 1 < route.waypoints.size(); ++i) {
            float sx1, sy1, sx2, sy2;
            canvas.WorldToScreen(route.waypoints[i].x, route.waypoints[i].y, sx1, sy1);
            canvas.WorldToScreen(route.waypoints[i+1].x, route.waypoints[i+1].y, sx2, sy2);
            dl->AddLine(ImVec2(sx1, sy1), ImVec2(sx2, sy2), color, thickness);
        }

        // Loop closing line (dashed via short segments)
        if (route.loop && route.waypoints.size() >= 2) {
            const auto& last = route.waypoints.back();
            const auto& first = route.waypoints.front();
            float sx1, sy1, sx2, sy2;
            canvas.WorldToScreen(last.x, last.y, sx1, sy1);
            canvas.WorldToScreen(first.x, first.y, sx2, sy2);

            // Dashed line: draw short segments
            float dx = sx2 - sx1, dy = sy2 - sy1;
            float len = std::sqrt(dx * dx + dy * dy);
            if (len > 0) {
                float dashLen = 8.0f, gapLen = 6.0f;
                float t = 0;
                while (t < len) {
                    float t1 = t / len;
                    float t2 = std::min((t + dashLen) / len, 1.0f);
                    dl->AddLine(
                        ImVec2(sx1 + dx * t1, sy1 + dy * t1),
                        ImVec2(sx1 + dx * t2, sy1 + dy * t2),
                        color, thickness);
                    t += dashLen + gapLen;
                }
            }
        }

        // Draw waypoint circles with numbers
        for (int i = 0; i < static_cast<int>(route.waypoints.size()); ++i) {
            float sx, sy;
            canvas.WorldToScreen(route.waypoints[i].x, route.waypoints[i].y, sx, sy);

            float radius = 6.0f;
            bool isSelected = (ri == selectedRoute && i == selectedWp);

            if (isSelected) {
                dl->AddCircleFilled(ImVec2(sx, sy), radius + 3, IM_COL32(255, 255, 0, 200));
            }

            dl->AddCircleFilled(ImVec2(sx, sy), radius, color);
            dl->AddCircle(ImVec2(sx, sy), radius, IM_COL32(0, 0, 0, 200), 0, 1.5f);

            // Number label
            if (canvas.zoom >= 0.04f) {
                char buf[8];
                snprintf(buf, sizeof(buf), "%d", i + 1);
                dl->AddText(ImVec2(sx + radius + 2, sy - 6), IM_COL32(220, 220, 220, 200), buf);
            }
        }
    }

    ImGui::End();
}

} // namespace mapedit
