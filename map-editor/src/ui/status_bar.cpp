#include "status_bar.h"
#include "../canvas/canvas.h"
#include "../camera/camera3d.h"
#include <imgui.h>
#include <cmath>

namespace mapedit {

static constexpr float kTileSize = 533.33333f;

void StatusBar::Render(const Canvas& canvas, uint32_t mapId, const char* mapName) {
    float barHeight = 28.0f;
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x, vp->WorkPos.y + vp->WorkSize.y - barHeight));
    ImGui::SetNextWindowSize(ImVec2(vp->WorkSize.x, barHeight));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8, 4));
    ImGui::Begin("##StatusBar", nullptr,
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus);

    // Mouse world position
    auto& io = ImGui::GetIO();
    float wx, wy;
    canvas.ScreenToWorld(io.MousePos.x, io.MousePos.y, wx, wy);

    int tileX = 31 - static_cast<int>(std::floor(wx / kTileSize));
    int tileY = 31 - static_cast<int>(std::floor(wy / kTileSize));

    ImGui::Text("Mode: 2D  |  Map: %s (%u)  |  Cursor: (%.1f, %.1f)  |  Tile: [%d, %d]  |  Zoom: %.3f",
                mapName ? mapName : "None", mapId, wx, wy, tileX, tileY, canvas.zoom);

    ImGui::End();
    ImGui::PopStyleVar();
}

void StatusBar::Render3D(const Camera3D& camera, uint32_t mapId, const char* mapName) {
    float barHeight = 28.0f;
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x, vp->WorkPos.y + vp->WorkSize.y - barHeight));
    ImGui::SetNextWindowSize(ImVec2(vp->WorkSize.x, barHeight));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8, 4));
    ImGui::Begin("##StatusBar", nullptr,
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus);

    if (camera.cameraMode == Camera3D::CameraMode::Free) {
        int tileX = 31 - static_cast<int>(std::floor(camera.eyeX / kTileSize));
        int tileY = 31 - static_cast<int>(std::floor(camera.eyeY / kTileSize));

        ImGui::Text("Mode: Free  |  Map: %s (%u)  |  Eye: (%.1f, %.1f, %.1f)  |  Tile: [%d, %d]  |  Speed: %.0f  |  Yaw: %.1f  Pitch: %.1f",
                    mapName ? mapName : "None", mapId,
                    camera.eyeX, camera.eyeY, camera.eyeZ,
                    tileX, tileY,
                    camera.moveSpeed,
                    camera.yaw * 180.0f / 3.14159f,
                    camera.pitch * 180.0f / 3.14159f);
    } else {
        int tileX = 31 - static_cast<int>(std::floor(camera.targetX / kTileSize));
        int tileY = 31 - static_cast<int>(std::floor(camera.targetY / kTileSize));

        ImGui::Text("Mode: 3D  |  Map: %s (%u)  |  Target: (%.1f, %.1f, %.1f)  |  Tile: [%d, %d]  |  Dist: %.0f  |  Yaw: %.1f  Pitch: %.1f",
                    mapName ? mapName : "None", mapId,
                    camera.targetX, camera.targetY, camera.targetZ,
                    tileX, tileY,
                    camera.distance,
                    camera.yaw * 180.0f / 3.14159f,
                    camera.pitch * 180.0f / 3.14159f);
    }

    ImGui::End();
    ImGui::PopStyleVar();
}

} // namespace mapedit
