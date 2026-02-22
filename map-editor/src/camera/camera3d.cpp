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
    // Eye position = target - spherical offset (camera SOUTH of target at yaw=0)
    // so the camera looks north (+X), matching the 2D canvas north-up orientation.
    float cosPitch = cosf(pitch);
    eyeX = targetX - distance * cosPitch * cosf(yaw);
    eyeY = targetY + distance * cosPitch * sinf(yaw);
    eyeZ = targetZ + distance * (-sinf(pitch)); // negative pitch = above target

    // Build View matrix using DirectXMath LookAtLH
    // WoW is Z-up. LookAtLH with up=(0,0,1) handles the coordinate mapping.
    XMVECTOR eye    = XMVectorSet(eyeX, eyeY, eyeZ, 1.0f);
    XMVECTOR target = XMVectorSet(targetX, targetY, targetZ, 1.0f);
    XMVECTOR up     = XMVectorSet(0.0f, 0.0f, 1.0f, 0.0f);

    XMMATRIX view = XMMatrixLookAtLH(eye, target, up);

    // Perspective projection
    float aspect = (vpH > 0.0f) ? (vpW / vpH) : 1.0f;
    float fovRad = fovY * XM_PI / 180.0f;
    XMMATRIX proj = XMMatrixPerspectiveFovLH(fovRad, aspect, nearPlane, farPlane);

    // VP = View * Proj.
    // Flip clip-space X so east (-Y) appears screen-right, matching 2D canvas.
    // (Camera looks north; LookAtLH gives xaxis=+Y=west as screen-right,
    //  the X-flip corrects this to east=right.)
    XMMATRIX vp = view * proj * XMMatrixScaling(-1.0f, 1.0f, 1.0f);
    // Transposed for HLSL column-major upload
    XMStoreFloat4x4(&viewProj, XMMatrixTranspose(vp));
}

// ---------------------------------------------------------------------------
// ProcessInput
// ---------------------------------------------------------------------------

void Camera3D::ProcessInput() {
    auto& io = ImGui::GetIO();

    // Only process input when mouse is inside the 3D viewport
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

            // Compute camera-relative right and up vectors in world space
            // Right = cross(forward, worldUp), normalized in XY plane
            float fwdX = targetX - eyeX;
            float fwdY = targetY - eyeY;
            float fwdLen = sqrtf(fwdX * fwdX + fwdY * fwdY);
            if (fwdLen < 0.001f) fwdLen = 0.001f;
            fwdX /= fwdLen;
            fwdY /= fwdLen;

            // Right = (-fwdY, fwdX) in XY plane (left-handed cross with Z-up)
            float rightX = -fwdY;
            float rightY =  fwdX;

            // Scale by distance for consistent pan speed
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
