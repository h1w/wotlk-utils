#include "path_renderer.h"
#include "../canvas/canvas.h"
#include "../navmesh/tile_cache.h"
#include "../navmesh/pathfinder.h"
#include <imgui.h>
#include <cstdio>
#include <cmath>

namespace mapedit {

void PathRenderer::Reset() {
    m_state = PathTestState::Idle;
    m_hasA = m_hasB = false;
    m_path.valid = false;
}

void PathRenderer::Render(const Canvas& canvas) {
    auto* dl = ImGui::GetForegroundDrawList();

    // Draw point A
    if (m_hasA) {
        float sx, sy;
        canvas.WorldToScreen(m_ax, m_ay, sx, sy);
        dl->AddCircleFilled(ImVec2(sx, sy), 8.0f, IM_COL32(255, 50, 50, 220));
        dl->AddText(ImVec2(sx + 10, sy - 8), IM_COL32(255, 100, 100, 255), "A");
    }

    // Draw point B
    if (m_hasB) {
        float sx, sy;
        canvas.WorldToScreen(m_bx, m_by, sx, sy);
        dl->AddCircleFilled(ImVec2(sx, sy), 8.0f, IM_COL32(50, 255, 50, 220));
        dl->AddText(ImVec2(sx + 10, sy - 8), IM_COL32(100, 255, 100, 255), "B");
    }

    // Draw path
    if (m_path.valid && m_path.pts.size() >= 2) {
        ImU32 pathColor = m_path.partial ? IM_COL32(255, 165, 0, 220) : IM_COL32(255, 255, 0, 220);

        for (size_t i = 0; i + 1 < m_path.pts.size(); ++i) {
            float sx1, sy1, sx2, sy2;
            canvas.WorldToScreen(m_path.pts[i].x, m_path.pts[i].y, sx1, sy1);
            canvas.WorldToScreen(m_path.pts[i+1].x, m_path.pts[i+1].y, sx2, sy2);
            dl->AddLine(ImVec2(sx1, sy1), ImVec2(sx2, sy2), pathColor, 3.0f);
        }

        // Distance label at midpoint
        size_t mid = m_path.pts.size() / 2;
        float lsx, lsy;
        canvas.WorldToScreen(m_path.pts[mid].x, m_path.pts[mid].y, lsx, lsy);
        char buf[64];
        snprintf(buf, sizeof(buf), "%.0f yd%s", m_path.distance,
                 m_path.partial ? " (partial)" : "");
        dl->AddText(ImVec2(lsx + 5, lsy - 16), IM_COL32(255, 255, 200, 255), buf);
    }

    // State indicator
    if (m_state == PathTestState::SetA) {
        ImVec2 mouse = ImGui::GetIO().MousePos;
        dl->AddCircle(ImVec2(mouse.x, mouse.y), 12.0f, IM_COL32(255, 50, 50, 180), 0, 2.0f);
    } else if (m_state == PathTestState::SetB) {
        ImVec2 mouse = ImGui::GetIO().MousePos;
        dl->AddCircle(ImVec2(mouse.x, mouse.y), 12.0f, IM_COL32(50, 255, 50, 180), 0, 2.0f);
    }
}

bool PathRenderer::ProcessInput(const Canvas& canvas, TileCache& cache, MapPathfinder& pathfinder) {
    if (m_state == PathTestState::Idle)
        return false;

    if (ImGui::GetIO().WantCaptureMouse)
        return false;

    ImVec2 mouse = ImGui::GetIO().MousePos;
    if (mouse.x < canvas.vpX || mouse.x > canvas.vpX + canvas.vpW ||
        mouse.y < canvas.vpY || mouse.y > canvas.vpY + canvas.vpH)
        return false;

    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        float wx, wy;
        canvas.ScreenToWorld(mouse.x, mouse.y, wx, wy);

        if (m_state == PathTestState::SetA) {
            m_ax = wx; m_ay = wy; m_az = 0;
            m_hasA = true;
            m_state = PathTestState::SetB;
            m_path.valid = false;
            return true;
        }

        if (m_state == PathTestState::SetB) {
            m_bx = wx; m_by = wy; m_bz = 0;
            m_hasB = true;
            m_state = PathTestState::Idle;

            // Find path
            auto* query = cache.GetQuery();
            if (query) {
                auto res = pathfinder.FindPath(query, m_ax, m_ay, m_az, m_bx, m_by, m_bz);
                m_path.valid = res.success;
                m_path.partial = res.partial;
                m_path.distance = res.totalDistance;
                m_path.pts.clear();
                for (const auto& wp : res.waypoints)
                    m_path.pts.push_back({wp.x, wp.y});
            }

            return true;
        }
    }

    // Right-click to cancel
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
        m_state = PathTestState::Idle;
        return true;
    }

    return false;
}

} // namespace mapedit
