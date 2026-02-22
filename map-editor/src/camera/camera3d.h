#pragma once

#include <DirectXMath.h>

namespace mapedit {

struct Camera3D {
    // Target point the camera orbits around (WoW world coords)
    float targetX = 0.0f;   // WoW X
    float targetY = 0.0f;   // WoW Y
    float targetZ = 100.0f; // WoW Z

    // Orbit parameters
    float distance = 200.0f;  // distance from target (yards)
    float yaw      = 0.0f;   // horizontal rotation (radians, 0 = south/WoW +X)
    float pitch    = -0.6f;  // vertical angle (radians, negative = looking down)

    // Limits
    static constexpr float kMinDist  = 10.0f;
    static constexpr float kMaxDist  = 5000.0f;
    static constexpr float kMinPitch = -1.5f;   // nearly straight down
    static constexpr float kMaxPitch = -0.05f;   // nearly horizontal

    // Viewport (set each frame from ImGui workspace)
    float vpX = 0.0f, vpY = 0.0f;
    float vpW = 800.0f, vpH = 600.0f;

    // Projection
    float nearPlane = 1.0f;
    float farPlane  = 10000.0f;
    float fovY      = 60.0f; // degrees

    // Eye position (computed by ComputeMatrices)
    float eyeX = 0.0f, eyeY = 0.0f, eyeZ = 0.0f;

    // Cached VP matrix (transposed for HLSL column-major upload)
    DirectX::XMFLOAT4X4 viewProj;

    // Compute View and Projection matrices.
    // Camera is placed NORTH of target (negated X offset) so it looks south,
    // matching the 2D canvas south-up orientation.
    // Stores transposed VP in viewProj for HLSL upload. Updates eyeX/Y/Z.
    void ComputeMatrices();

    // Process input: right-click drag orbit, scroll zoom, middle-click pan
    void ProcessInput();

    // Project world coords to screen coords. Returns false if behind camera.
    bool WorldToScreen(float wx, float wy, float wz,
                       float& sx, float& sy) const;

    // Extract 6 frustum planes from VP matrix (for culling)
    // planes[i] = (A, B, C, D) where Ax+By+Cz+D >= 0 means inside
    void GetFrustumPlanes(float planes[6][4]) const;

    // Follow mode (tracks player position)
    bool followMode = false;
    float followDistance = 30.0f;

    // Update follow target from player position
    void UpdateFollow(float playerX, float playerY, float playerZ);

    // Unproject screen coordinates to a world-space ray
    void ScreenToRay(float sx, float sy,
                     float& originX, float& originY, float& originZ,
                     float& dirX, float& dirY, float& dirZ) const;

private:
    // Pan state
    bool  m_panning = false;
    float m_panStartMouseX = 0.0f, m_panStartMouseY = 0.0f;
    float m_panStartTargetX = 0.0f, m_panStartTargetY = 0.0f, m_panStartTargetZ = 0.0f;

    // Orbit state
    bool  m_orbiting = false;
    float m_orbitStartMouseX = 0.0f, m_orbitStartMouseY = 0.0f;
    float m_orbitStartYaw = 0.0f, m_orbitStartPitch = 0.0f;
};

} // namespace mapedit
