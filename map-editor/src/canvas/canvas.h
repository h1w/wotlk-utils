#pragma once

struct ImVec2;

namespace mapedit {

struct Canvas {
    // Center of the view in WoW world coordinates
    float centerX = 0.0f; // WoW X (north+)
    float centerY = 0.0f; // WoW Y (west+)
    float zoom    = 0.1f; // pixels per yard

    // Pan state
    bool  panning  = false;
    float panStartMouseX = 0.0f;
    float panStartMouseY = 0.0f;
    float panStartCenterX = 0.0f;
    float panStartCenterY = 0.0f;

    // Viewport (set each frame)
    float vpX = 0.0f, vpY = 0.0f; // top-left of canvas area in screen coords
    float vpW = 0.0f, vpH = 0.0f; // size of canvas area

    // Convert WoW world coords to screen pixel coords (north-up view)
    void WorldToScreen(float wowX, float wowY, float& sx, float& sy) const;

    // Convert screen pixel coords to WoW world coords
    void ScreenToWorld(float sx, float sy, float& wowX, float& wowY) const;

    // Process input: pan (middle-click drag), zoom (scroll wheel centered on cursor)
    void ProcessInput();

    // Get the world-space bounding box of the current viewport
    void GetViewBounds(float& minX, float& maxX, float& minY, float& maxY) const;
};

} // namespace mapedit
