#include "camera3d.h"

#include <imgui.h>
#include <cmath>
#include <algorithm>

using namespace DirectX;

namespace mapedit {

// ---------------------------------------------------------------------------
// ComputeMatrices
// ---------------------------------------------------------------------------

void Camera3D::ComputeMatrices() {
    if (cameraMode == CameraMode::Free) {
        // Free camera: eye IS the position; look direction from yaw/pitch
        float cosPitch = cosf(pitch);
        float lookX =  cosPitch * cosf(yaw);   // north at yaw=0
        float lookY = -cosPitch * sinf(yaw);
        float lookZ =  sinf(pitch);

        float atX = eyeX + lookX;
        float atY = eyeY + lookY;
        float atZ = eyeZ + lookZ;

        XMVECTOR eye    = XMVectorSet(eyeX, eyeY, eyeZ, 1.0f);
        XMVECTOR target = XMVectorSet(atX, atY, atZ, 1.0f);
        XMVECTOR up     = XMVectorSet(0.0f, 0.0f, 1.0f, 0.0f);

        XMMATRIX view = XMMatrixLookAtLH(eye, target, up);

        float aspect = (vpH > 0.0f) ? (vpW / vpH) : 1.0f;
        float fovRad = fovY * XM_PI / 180.0f;
        XMMATRIX proj = XMMatrixPerspectiveFovLH(fovRad, aspect, nearPlane, farPlane);

        XMMATRIX vp = view * proj * XMMatrixScaling(-1.0f, 1.0f, 1.0f);
        XMStoreFloat4x4(&viewProj, XMMatrixTranspose(vp));

        // Sync target for consumers (tile loading, LOD, etc.)
        targetX = eyeX + lookX * distance;
        targetY = eyeY + lookY * distance;
        targetZ = eyeZ + lookZ * distance;
    } else {
        // Orbit camera: eye orbits around target
        float cosPitch = cosf(pitch);
        eyeX = targetX - distance * cosPitch * cosf(yaw);
        eyeY = targetY + distance * cosPitch * sinf(yaw);
        eyeZ = targetZ + distance * (-sinf(pitch));

        XMVECTOR eye    = XMVectorSet(eyeX, eyeY, eyeZ, 1.0f);
        XMVECTOR target = XMVectorSet(targetX, targetY, targetZ, 1.0f);
        XMVECTOR up     = XMVectorSet(0.0f, 0.0f, 1.0f, 0.0f);

        XMMATRIX view = XMMatrixLookAtLH(eye, target, up);

        float aspect = (vpH > 0.0f) ? (vpW / vpH) : 1.0f;
        float fovRad = fovY * XM_PI / 180.0f;
        XMMATRIX proj = XMMatrixPerspectiveFovLH(fovRad, aspect, nearPlane, farPlane);

        XMMATRIX vp = view * proj * XMMatrixScaling(-1.0f, 1.0f, 1.0f);
        XMStoreFloat4x4(&viewProj, XMMatrixTranspose(vp));
    }
}

// ---------------------------------------------------------------------------
// ProcessInput
// ---------------------------------------------------------------------------

void Camera3D::ProcessInput() {
    if (cameraMode == CameraMode::Free)
        ProcessInputFree();
    else
        ProcessInputOrbit();
}

// ---------------------------------------------------------------------------
// SetCameraMode
// ---------------------------------------------------------------------------

void Camera3D::SetCameraMode(CameraMode mode) {
    if (mode == cameraMode) return;

    if (mode == CameraMode::Free) {
        // Orbit → Free: eyeX/Y/Z and yaw/pitch are already valid, nothing to do
    } else {
        // Free → Orbit: compute target as point 'distance' ahead of eye
        float cosPitch = cosf(pitch);
        float lookX =  cosPitch * cosf(yaw);
        float lookY = -cosPitch * sinf(yaw);
        float lookZ =  sinf(pitch);
        targetX = eyeX + lookX * distance;
        targetY = eyeY + lookY * distance;
        targetZ = eyeZ + lookZ * distance;

        // Clamp pitch to orbit range
        pitch = std::clamp(pitch, kMinPitch, kMaxPitch);
    }

    // Reset interaction flags
    m_orbiting = false;
    m_panning = false;
    m_freeLooking = false;

    cameraMode = mode;
}

// ---------------------------------------------------------------------------
// ProcessInputOrbit
// ---------------------------------------------------------------------------

void Camera3D::ProcessInputOrbit() {
    auto& io = ImGui::GetIO();

    bool mouseInViewport = (io.MousePos.x >= vpX && io.MousePos.x <= vpX + vpW &&
                            io.MousePos.y >= vpY && io.MousePos.y <= vpY + vpH);

    // Orbit: right-click drag
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Right) && mouseInViewport && !io.WantCaptureMouse) {
        m_orbiting = true;
        m_orbitStartMouseX = io.MousePos.x;
        m_orbitStartMouseY = io.MousePos.y;
        m_orbitStartYaw    = yaw;
        m_orbitStartPitch  = pitch;
    }
    if (m_orbiting) {
        if (ImGui::IsMouseDown(ImGuiMouseButton_Right)) {
            float dx = io.MousePos.x - m_orbitStartMouseX;
            float dy = io.MousePos.y - m_orbitStartMouseY;
            float sensitivity = 0.005f;
            yaw   = m_orbitStartYaw   + dx * sensitivity;
            pitch = m_orbitStartPitch + dy * sensitivity;
            pitch = std::clamp(pitch, kMinPitch, kMaxPitch);
        } else {
            m_orbiting = false;
        }
    }

    // Pan: middle-click drag
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Middle) && mouseInViewport && !io.WantCaptureMouse) {
        m_panning = true;
        m_panStartMouseX  = io.MousePos.x;
        m_panStartMouseY  = io.MousePos.y;
        m_panStartTargetX = targetX;
        m_panStartTargetY = targetY;
        m_panStartTargetZ = targetZ;
    }
    if (m_panning) {
        if (ImGui::IsMouseDown(ImGuiMouseButton_Middle)) {
            float dx = io.MousePos.x - m_panStartMouseX;
            float dy = io.MousePos.y - m_panStartMouseY;

            float fwdX = targetX - eyeX;
            float fwdY = targetY - eyeY;
            float fwdLen = sqrtf(fwdX * fwdX + fwdY * fwdY);
            if (fwdLen < 0.001f) fwdLen = 0.001f;
            fwdX /= fwdLen;
            fwdY /= fwdLen;

            float rightX = -fwdY;
            float rightY =  fwdX;

            float panScale = distance * 0.002f;
            targetX = m_panStartTargetX + (dx * rightX) * panScale + (dy * fwdX) * panScale;
            targetY = m_panStartTargetY + (dx * rightY) * panScale + (dy * fwdY) * panScale;
        } else {
            m_panning = false;
        }
    }

    // Zoom: scroll wheel
    if (mouseInViewport && !io.WantCaptureMouse && io.MouseWheel != 0.0f) {
        float factor = 1.0f - io.MouseWheel * 0.1f;
        distance *= factor;
        distance = std::clamp(distance, kMinDist, kMaxDist);
    }
}

// ---------------------------------------------------------------------------
// ProcessInputFree
// ---------------------------------------------------------------------------

void Camera3D::ProcessInputFree() {
    auto& io = ImGui::GetIO();
    float dt = io.DeltaTime;

    bool mouseInViewport = (io.MousePos.x >= vpX && io.MousePos.x <= vpX + vpW &&
                            io.MousePos.y >= vpY && io.MousePos.y <= vpY + vpH);

    // Mouse look: right-click drag
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Right) && mouseInViewport && !io.WantCaptureMouse) {
        m_freeLooking = true;
        m_freeLookStartMouseX = io.MousePos.x;
        m_freeLookStartMouseY = io.MousePos.y;
        m_freeLookStartYaw    = yaw;
        m_freeLookStartPitch  = pitch;
    }
    if (m_freeLooking) {
        if (ImGui::IsMouseDown(ImGuiMouseButton_Right)) {
            float dx = io.MousePos.x - m_freeLookStartMouseX;
            float dy = io.MousePos.y - m_freeLookStartMouseY;
            float sensitivity = 0.005f;
            yaw   = m_freeLookStartYaw   + dx * sensitivity;
            pitch = m_freeLookStartPitch + dy * sensitivity;
            pitch = std::clamp(pitch, kFreePitchMin, kFreePitchMax);
        } else {
            m_freeLooking = false;
        }
    }

    // Scroll wheel: adjust move speed
    if (mouseInViewport && !io.WantCaptureMouse && io.MouseWheel != 0.0f) {
        float factor = powf(1.15f, io.MouseWheel);
        moveSpeed *= factor;
        moveSpeed = std::clamp(moveSpeed, kMinMoveSpeed, kMaxMoveSpeed);
    }

    // WASD movement (only when not typing in a text field)
    if (!io.WantTextInput) {
        // Forward direction in XY plane (north at yaw=0)
        float fwdX =  cosf(yaw);
        float fwdY = -sinf(yaw);
        // Right direction (east = -Y at yaw=0)
        float rightX = -sinf(yaw);
        float rightY = -cosf(yaw);

        float moveX = 0.0f, moveY = 0.0f, moveZ = 0.0f;

        if (ImGui::IsKeyDown(ImGuiKey_W)) { moveX += fwdX;   moveY += fwdY; }
        if (ImGui::IsKeyDown(ImGuiKey_S)) { moveX -= fwdX;   moveY -= fwdY; }
        if (ImGui::IsKeyDown(ImGuiKey_A)) { moveX -= rightX; moveY -= rightY; }
        if (ImGui::IsKeyDown(ImGuiKey_D)) { moveX += rightX; moveY += rightY; }

        if (ImGui::IsKeyDown(ImGuiKey_Space) || ImGui::IsKeyDown(ImGuiKey_Q))
            moveZ += 1.0f;
        if (ImGui::IsKeyDown(ImGuiKey_LeftCtrl) || ImGui::IsKeyDown(ImGuiKey_E))
            moveZ -= 1.0f;

        // Normalize horizontal
        float hLen = sqrtf(moveX * moveX + moveY * moveY);
        if (hLen > 1.0f) {
            moveX /= hLen;
            moveY /= hLen;
        }

        float speed = moveSpeed;
        if (ImGui::IsKeyDown(ImGuiKey_LeftShift))
            speed *= 3.0f;

        eyeX += moveX * speed * dt;
        eyeY += moveY * speed * dt;
        eyeZ += moveZ * speed * dt;
    }
}

// ---------------------------------------------------------------------------
// WorldToScreen
// ---------------------------------------------------------------------------

bool Camera3D::WorldToScreen(float wx, float wy, float wz,
                              float& sx, float& sy) const {
    // Load the VP matrix (stored transposed for HLSL, transpose back to get real VP)
    XMMATRIX vp = XMLoadFloat4x4(&viewProj);
    vp = XMMatrixTranspose(vp);

    XMVECTOR worldPos = XMVectorSet(wx, wy, wz, 1.0f);
    XMVECTOR clipPos  = XMVector4Transform(worldPos, vp);

    float w = XMVectorGetW(clipPos);
    if (w <= 0.0f) return false; // behind camera

    float ndcX = XMVectorGetX(clipPos) / w;
    float ndcY = XMVectorGetY(clipPos) / w;

    // NDC [-1,1] -> screen coords
    sx = vpX + (ndcX * 0.5f + 0.5f) * vpW;
    sy = vpY + (-ndcY * 0.5f + 0.5f) * vpH; // flip Y (NDC Y-up, screen Y-down)

    return true;
}

// ---------------------------------------------------------------------------
// GetFrustumPlanes
// ---------------------------------------------------------------------------

void Camera3D::GetFrustumPlanes(float planes[6][4]) const {
    // Extract frustum planes from VP matrix (non-transposed)
    XMMATRIX vp = XMLoadFloat4x4(&viewProj);
    vp = XMMatrixTranspose(vp); // undo HLSL transpose to get real VP

    XMFLOAT4X4 m;
    XMStoreFloat4x4(&m, vp);

    // Left:   row3 + row0
    planes[0][0] = m._14 + m._11;
    planes[0][1] = m._24 + m._21;
    planes[0][2] = m._34 + m._31;
    planes[0][3] = m._44 + m._41;

    // Right:  row3 - row0
    planes[1][0] = m._14 - m._11;
    planes[1][1] = m._24 - m._21;
    planes[1][2] = m._34 - m._31;
    planes[1][3] = m._44 - m._41;

    // Bottom: row3 + row1
    planes[2][0] = m._14 + m._12;
    planes[2][1] = m._24 + m._22;
    planes[2][2] = m._34 + m._32;
    planes[2][3] = m._44 + m._42;

    // Top:    row3 - row1
    planes[3][0] = m._14 - m._12;
    planes[3][1] = m._24 - m._22;
    planes[3][2] = m._34 - m._32;
    planes[3][3] = m._44 - m._42;

    // Near:   row3 + row2
    planes[4][0] = m._14 + m._13;
    planes[4][1] = m._24 + m._23;
    planes[4][2] = m._34 + m._33;
    planes[4][3] = m._44 + m._43;

    // Far:    row3 - row2
    planes[5][0] = m._14 - m._13;
    planes[5][1] = m._24 - m._23;
    planes[5][2] = m._34 - m._33;
    planes[5][3] = m._44 - m._43;

    // Normalize each plane
    for (int i = 0; i < 6; ++i) {
        float len = sqrtf(planes[i][0] * planes[i][0] +
                          planes[i][1] * planes[i][1] +
                          planes[i][2] * planes[i][2]);
        if (len > 0.0f) {
            planes[i][0] /= len;
            planes[i][1] /= len;
            planes[i][2] /= len;
            planes[i][3] /= len;
        }
    }
}

// ---------------------------------------------------------------------------
// UpdateFollow
// ---------------------------------------------------------------------------

void Camera3D::UpdateFollow(float playerX, float playerY, float playerZ) {
    if (!followMode) return;
    if (cameraMode == CameraMode::Free) return;

    targetX = playerX;
    targetY = playerY;
    targetZ = playerZ + 2.0f; // slightly above feet

    if (followDistance > 0.0f) {
        distance = followDistance;
    }
}

// ---------------------------------------------------------------------------
// ScreenToRay
// ---------------------------------------------------------------------------

void Camera3D::ScreenToRay(float sx, float sy,
                            float& originX, float& originY, float& originZ,
                            float& dirX, float& dirY, float& dirZ) const {
    // Convert screen coords to NDC
    float ndcX =  ((sx - vpX) / vpW * 2.0f - 1.0f);
    float ndcY = -((sy - vpY) / vpH * 2.0f - 1.0f); // flip Y (screen Y-down, NDC Y-up)

    // Load the VP matrix (stored transposed for HLSL, transpose back to get real VP)
    XMMATRIX vp = XMLoadFloat4x4(&viewProj);
    vp = XMMatrixTranspose(vp);

    // Invert the VP matrix
    XMVECTOR det;
    XMMATRIX invVP = XMMatrixInverse(&det, vp);

    // Unproject near point (NDC z=0) and far point (NDC z=1)
    XMVECTOR nearNDC = XMVectorSet(ndcX, ndcY, 0.0f, 1.0f);
    XMVECTOR farNDC  = XMVectorSet(ndcX, ndcY, 1.0f, 1.0f);

    XMVECTOR nearWorld = XMVector4Transform(nearNDC, invVP);
    XMVECTOR farWorld  = XMVector4Transform(farNDC,  invVP);

    // Perspective divide
    float nearW = XMVectorGetW(nearWorld);
    float farW  = XMVectorGetW(farWorld);

    nearWorld = XMVectorScale(nearWorld, 1.0f / nearW);
    farWorld  = XMVectorScale(farWorld,  1.0f / farW);

    // Ray origin = near point
    originX = XMVectorGetX(nearWorld);
    originY = XMVectorGetY(nearWorld);
    originZ = XMVectorGetZ(nearWorld);

    // Ray direction = normalize(far - near)
    XMVECTOR dir = XMVectorSubtract(farWorld, nearWorld);
    dir = XMVector3Normalize(dir);

    dirX = XMVectorGetX(dir);
    dirY = XMVectorGetY(dir);
    dirZ = XMVectorGetZ(dir);
}

} // namespace mapedit
