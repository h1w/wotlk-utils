#include "route_editor.h"
#include "undo_redo.h"
#include "../canvas/canvas.h"
#include "../data/route_data.h"
#include <imgui.h>
#include <cmath>
#include <cstring>

namespace mapedit {

void RouteEditor::RenderPanel(RouteData& data, uint32_t mapId, UndoContext& undo) {
    ImGui::Begin("Routes");

    if (ImGui::Button("New Route")) {
        undo.Snapshot("New Route");
        Route route;
        route.name = "New Route";
        route.mapId = mapId;
        data.AddRoute(route);
        m_selectedRoute = static_cast<int>(data.GetRoutes().size()) - 1;
    }

    ImGui::Separator();

    auto& routes = data.GetRoutes();
    for (int i = 0; i < static_cast<int>(routes.size()); ++i) {
        auto& route = routes[i];
        bool selected = (i == m_selectedRoute);

        ImGui::PushID(i);

        // Color indicator
        ImVec4 col(
            (route.color & 0xFF) / 255.0f,
            ((route.color >> 8) & 0xFF) / 255.0f,
            ((route.color >> 16) & 0xFF) / 255.0f,
            1.0f);
        ImGui::ColorButton("##c", col, ImGuiColorEditFlags_NoTooltip, ImVec2(14, 14));
        ImGui::SameLine();

        if (ImGui::Selectable(route.name.c_str(), selected)) {
            m_selectedRoute = i;
            m_selectedWaypoint = -1;
        }

        ImGui::PopID();
    }

    ImGui::Separator();

    if (m_selectedRoute >= 0 && m_selectedRoute < static_cast<int>(routes.size())) {
        auto& route = routes[m_selectedRoute];

        char nameBuf[128];
        strncpy(nameBuf, route.name.c_str(), sizeof(nameBuf) - 1);
        nameBuf[sizeof(nameBuf) - 1] = '\0';
        if (ImGui::InputText("Name", nameBuf, sizeof(nameBuf))) {
            undo.SnapshotIfNeeded("Edit Route Name");
            route.name = nameBuf;
            data.MarkDirty();
        }

        // Color picker (convert ABGR -> float4)
        float col[3] = {
            (route.color & 0xFF) / 255.0f,
            ((route.color >> 8) & 0xFF) / 255.0f,
            ((route.color >> 16) & 0xFF) / 255.0f
        };
        if (ImGui::ColorEdit3("Color", col)) {
            undo.SnapshotIfNeeded("Edit Route Color");
            route.color = 0xFF000000
                | (static_cast<uint32_t>(col[2] * 255) << 16)
                | (static_cast<uint32_t>(col[1] * 255) << 8)
                | static_cast<uint32_t>(col[0] * 255);
            data.MarkDirty();
        }

        {
            bool prevLoop = route.loop;
            if (ImGui::Checkbox("Loop", &route.loop)) {
                bool newLoop = route.loop;
                route.loop = prevLoop;
                undo.Snapshot("Edit Route Loop");
                route.loop = newLoop;
                data.MarkDirty();
            }
        }

        ImGui::Text("Waypoints: %d", static_cast<int>(route.waypoints.size()));

        if (ImGui::Checkbox("Edit Mode", &m_editing)) {
            m_selectedWaypoint = -1;
        }

        ImGui::Separator();

        if (ImGui::Button("Delete Route")) {
            undo.Snapshot("Delete Route");
            data.RemoveRoute(m_selectedRoute);
            m_selectedRoute = -1;
            m_selectedWaypoint = -1;
            m_editing = false;
        }
    }

    ImGui::End();
}

bool RouteEditor::ProcessInput(const Canvas& canvas, RouteData& data, uint32_t mapId,
                                UndoContext& undo) {
    if (!m_editing || m_selectedRoute < 0)
        return false;

    if (ImGui::GetIO().WantCaptureMouse)
        return false;

    auto& routes = data.GetRoutes();
    if (m_selectedRoute >= static_cast<int>(routes.size()))
        return false;

    auto& route = routes[m_selectedRoute];
    ImVec2 mouse = ImGui::GetIO().MousePos;
    float mx = mouse.x, my = mouse.y;

    // Check viewport bounds
    if (mx < canvas.vpX || mx > canvas.vpX + canvas.vpW ||
        my < canvas.vpY || my > canvas.vpY + canvas.vpH)
        return false;

    // Dragging waypoint
    if (m_draggingWaypoint && m_selectedWaypoint >= 0) {
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            if (!m_dragWpSnapshotTaken) {
                ImVec2 delta = ImGui::GetMouseDragDelta(ImGuiMouseButton_Left, 2.0f);
                if (delta.x != 0.0f || delta.y != 0.0f) {
                    undo.Snapshot("Move Waypoint");
                    m_dragWpSnapshotTaken = true;
                }
            }
            if (m_dragWpSnapshotTaken) {
                auto& wp = route.waypoints[m_selectedWaypoint];
                canvas.ScreenToWorld(mx, my, wp.x, wp.y);
                data.MarkDirty();
            }
            return true;
        } else {
            m_draggingWaypoint = false;
        }
    }

    // Hit test existing waypoints
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        int hitIdx = -1;
        float hitDist = 10.0f;

        for (int i = 0; i < static_cast<int>(route.waypoints.size()); ++i) {
            float sx, sy;
            canvas.WorldToScreen(route.waypoints[i].x, route.waypoints[i].y, sx, sy);
            float dx = sx - mx, dy = sy - my;
            float d = std::sqrt(dx * dx + dy * dy);
            if (d < hitDist) {
                hitDist = d;
                hitIdx = i;
            }
        }

        if (hitIdx >= 0) {
            m_selectedWaypoint = hitIdx;
            m_draggingWaypoint = true;
            m_dragWpSnapshotTaken = false;
            return true;
        }

        // Click on empty space -- add waypoint at end
        undo.Snapshot("Add Waypoint");
        RouteWaypoint wp;
        canvas.ScreenToWorld(mx, my, wp.x, wp.y);
        wp.z = 0;
        route.waypoints.push_back(wp);
        m_selectedWaypoint = static_cast<int>(route.waypoints.size()) - 1;
        data.MarkDirty();
        return true;
    }

    // Right-click to delete selected waypoint
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Right) && m_selectedWaypoint >= 0) {
        if (m_selectedWaypoint < static_cast<int>(route.waypoints.size())) {
            undo.Snapshot("Delete Waypoint");
            route.waypoints.erase(route.waypoints.begin() + m_selectedWaypoint);
            m_selectedWaypoint = -1;
            data.MarkDirty();
            return true;
        }
    }

    return false;
}

} // namespace mapedit
