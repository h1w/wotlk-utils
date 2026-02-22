#include "canvas.h"
#include <imgui.h>
#include <algorithm>
#include <cmath>

namespace mapedit {

void Canvas::WorldToScreen(float wowX, float wowY, float& sx, float& sy) const {
    // North-up: screen_right = East (-Y), screen_down = South (-X)
    sx = (vpX + vpW * 0.5f) + (centerY - wowY) * zoom;
    sy = (vpY + vpH * 0.5f) + (centerX - wowX) * zoom;
}

void Canvas::ScreenToWorld(float sx, float sy, float& wowX, float& wowY) const {
    float cx = vpX + vpW * 0.5f;
    float cy = vpY + vpH * 0.5f;
    wowY = centerY - (sx - cx) / zoom;
    wowX = centerX - (sy - cy) / zoom;
}

void Canvas::ProcessInput() {
    auto& io = ImGui::GetIO();
    ImVec2 mouse = io.MousePos;

    // Check if mouse is inside viewport
    bool inViewport = mouse.x >= vpX && mouse.x <= vpX + vpW &&
                      mouse.y >= vpY && mouse.y <= vpY + vpH;

    if (!inViewport) {
        if (panning && !ImGui::IsMouseDown(ImGuiMouseButton_Middle))
            panning = false;
        return;
    }

    // Don't process canvas input if ImGui wants the mouse (e.g., a window is hovered)
    if (ImGui::GetIO().WantCaptureMouse && !panning)
        return;

    // Pan: middle-click drag
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Middle)) {
        panning = true;
        panStartMouseX = mouse.x;
        panStartMouseY = mouse.y;
        panStartCenterX = centerX;
        panStartCenterY = centerY;
    }
    if (panning) {
        if (ImGui::IsMouseDown(ImGuiMouseButton_Middle)) {
            float dx = mouse.x - panStartMouseX;
            float dy = mouse.y - panStartMouseY;
            // Invert: dragging right moves view left (shows more east = -Y)
            centerY = panStartCenterY + dx / zoom;
            centerX = panStartCenterX + dy / zoom;
        } else {
            panning = false;
        }
    }

    // Zoom: scroll wheel centered on cursor
    float wheel = io.MouseWheel;
    if (wheel != 0.0f) {
        // Get world pos under cursor before zoom
        float worldX, worldY;
        ScreenToWorld(mouse.x, mouse.y, worldX, worldY);

        // Apply zoom
        float factor = (wheel > 0) ? 1.15f : (1.0f / 1.15f);
        zoom *= factor;
        zoom = std::clamp(zoom, 0.001f, 100.0f);

        // Adjust center so the world point under cursor stays under cursor
        // After zoom: sx = cx + (centerY - wowY) * newZoom
        // We want sx (mouse.x) to map to the same worldX, worldY
        float cx = vpX + vpW * 0.5f;
        float cy = vpY + vpH * 0.5f;
        centerY = worldY + (mouse.x - cx) / zoom;
        centerX = worldX + (mouse.y - cy) / zoom;
    }
}

void Canvas::GetViewBounds(float& minX, float& maxX, float& minY, float& maxY) const {
    // Screen corners -> world coords
    float tlX, tlY, brX, brY;
    ScreenToWorld(vpX, vpY, tlX, tlY);
    ScreenToWorld(vpX + vpW, vpY + vpH, brX, brY);
    minX = std::min(tlX, brX);
    maxX = std::max(tlX, brX);
    minY = std::min(tlY, brY);
    maxY = std::max(tlY, brY);
}

} // namespace mapedit
