#include "status_bar.h"
#include "issue_panel.h"
#include "../canvas/canvas.h"
#include "../camera/camera3d.h"
#include "../data/terrain_height_sampler.h"
#include <imgui.h>
#include <cmath>
#include <cstdio>

namespace mapedit {

static constexpr float kTileSize = 533.33333f;

// Format height string: "H: 123.4" or "H: ---" if unavailable
static void FormatHeight(char* buf, size_t bufSize, TerrainHeightSampler* sampler,
                         uint32_t mapId, float wx, float wy) {
    if (sampler) {
        auto h = sampler->SampleHeight(mapId, wx, wy);
        if (h.has_value())
            snprintf(buf, bufSize, "H: %.1f", h.value());
        else
            snprintf(buf, bufSize, "H: ---");
    } else {
        snprintf(buf, bufSize, "H: ---");
    }
}

void StatusBar::Render(const Canvas& canvas, uint32_t mapId, const char* mapName,
                       TerrainHeightSampler* heightSampler, IssuePanel* issuePanel) {
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

    char hBuf[32];
    FormatHeight(hBuf, sizeof(hBuf), heightSampler, mapId, wx, wy);

    ImGui::Text("Mode: 2D  |  Map: %s (%u)  |  Cursor: (%.1f, %.1f)  |  Tile: [%d, %d]  |  Zoom: %.3f  |  %s",
                mapName ? mapName : "None", mapId, wx, wy, tileX, tileY, canvas.zoom, hBuf);

    if (issuePanel)
        issuePanel->RenderIndicator();

    ImGui::End();
    ImGui::PopStyleVar();
}

void StatusBar::Render3D(const Camera3D& camera, uint32_t mapId, const char* mapName,
                         TerrainHeightSampler* heightSampler, IssuePanel* issuePanel) {
    float barHeight = 28.0f;
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x, vp->WorkPos.y + vp->WorkSize.y - barHeight));
    ImGui::SetNextWindowSize(ImVec2(vp->WorkSize.x, barHeight));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8, 4));
    ImGui::Begin("##StatusBar", nullptr,
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus);

    // Ray-cast cursor to ground plane to get world XY under cursor
    auto& io = ImGui::GetIO();
    float cursorWX = 0, cursorWY = 0;
    bool hasCursorWorld = false;
    {
        float ox, oy, oz, dx, dy, dz;
        camera.ScreenToRay(io.MousePos.x, io.MousePos.y, ox, oy, oz, dx, dy, dz);
        // Intersect with Z=targetZ plane (orbit) or Z=eyeZ-50 plane (free)
        float planeZ = (camera.cameraMode == Camera3D::CameraMode::Orbit)
                       ? camera.targetZ : (camera.eyeZ - 50.0f);
        if (std::abs(dz) > 1e-6f) {
            float t = (planeZ - oz) / dz;
            if (t > 0.0f) {
                cursorWX = ox + dx * t;
                cursorWY = oy + dy * t;
                hasCursorWorld = true;
            }
        }
    }

    char hBuf[32];
    if (hasCursorWorld)
        FormatHeight(hBuf, sizeof(hBuf), heightSampler, mapId, cursorWX, cursorWY);
    else
        snprintf(hBuf, sizeof(hBuf), "H: ---");

    if (camera.cameraMode == Camera3D::CameraMode::Free) {
        int tileX = 31 - static_cast<int>(std::floor(camera.eyeX / kTileSize));
        int tileY = 31 - static_cast<int>(std::floor(camera.eyeY / kTileSize));

        ImGui::Text("Mode: Free  |  Map: %s (%u)  |  Eye: (%.1f, %.1f, %.1f)  |  Tile: [%d, %d]  |  Speed: %.0f  |  Yaw: %.1f  Pitch: %.1f  |  %s",
                    mapName ? mapName : "None", mapId,
                    camera.eyeX, camera.eyeY, camera.eyeZ,
                    tileX, tileY,
                    camera.moveSpeed,
                    camera.yaw * 180.0f / 3.14159f,
                    camera.pitch * 180.0f / 3.14159f,
                    hBuf);
    } else {
        int tileX = 31 - static_cast<int>(std::floor(camera.targetX / kTileSize));
        int tileY = 31 - static_cast<int>(std::floor(camera.targetY / kTileSize));

        ImGui::Text("Mode: 3D  |  Map: %s (%u)  |  Target: (%.1f, %.1f, %.1f)  |  Tile: [%d, %d]  |  Dist: %.0f  |  Yaw: %.1f  Pitch: %.1f  |  %s",
                    mapName ? mapName : "None", mapId,
                    camera.targetX, camera.targetY, camera.targetZ,
                    tileX, tileY,
                    camera.distance,
                    camera.yaw * 180.0f / 3.14159f,
                    camera.pitch * 180.0f / 3.14159f,
                    hBuf);
    }

    if (issuePanel)
        issuePanel->RenderIndicator();

    ImGui::End();
    ImGui::PopStyleVar();
}

} // namespace mapedit
