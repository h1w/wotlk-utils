#include "compass.h"
#include <imgui.h>
#include <cmath>

namespace mapedit {

void CompassWidget::Render(float vpX, float vpY, float vpW, float vpH, float rotation) {
    constexpr float kRadius = 40.0f;
    constexpr float kMargin = 15.0f;
    constexpr float kTickInner = 0.55f;     // cardinal tick start (fraction of radius)
    constexpr float kTickOuter = 0.92f;     // cardinal tick end
    constexpr float kSubInner = 0.65f;      // intercardinal tick start
    constexpr float kSubOuter = 0.85f;      // intercardinal tick end
    constexpr float kLabelR = 1.18f;        // label distance (fraction of radius)
    constexpr float kBgAlpha = 0.45f;
    constexpr float kNorthTriR = 0.35f;     // north triangle inner radius

    const float cx = vpX + vpW - kMargin - kRadius;
    const float cy = vpY + kMargin + kRadius;

    auto* dl = ImGui::GetForegroundDrawList();

    // Background circle
    dl->AddCircleFilled(ImVec2(cx, cy), kRadius,
                        IM_COL32(20, 20, 25, (int)(255 * kBgAlpha)), 32);
    dl->AddCircle(ImVec2(cx, cy), kRadius,
                  IM_COL32(180, 180, 180, 100), 32, 1.5f);

    // Cardinal directions: base angles from screen-up, clockwise
    // In 2D (rotation=0): S=top(0), E=right(pi/2), N=bottom(pi), W=left(3pi/2)
    struct Cardinal {
        float baseAngle;    // radians from screen-up, clockwise
        const char* label;
        ImU32 color;
    };

    constexpr float kPi = 3.14159265f;
    const Cardinal cardinals[] = {
        { 0.0f,         "N", IM_COL32(255, 80, 80, 255) },
        { kPi * 0.5f,   "E", IM_COL32(255, 255, 255, 220) },
        { kPi,          "S", IM_COL32(255, 255, 255, 220) },
        { kPi * 1.5f,   "W", IM_COL32(255, 255, 255, 220) },
    };

    // Intercardinal base angles
    const float intercardinals[] = {
        kPi * 0.25f,    // SE
        kPi * 0.75f,    // NE
        kPi * 1.25f,    // NW
        kPi * 1.75f,    // SW
    };

    // Helper: angle to screen dx/dy (from top, clockwise)
    auto angleToDir = [](float a, float& dx, float& dy) {
        dx = sinf(a);
        dy = -cosf(a);
    };

    // Draw intercardinal ticks (thinner, behind cardinals)
    for (float base : intercardinals) {
        float a = base + rotation;
        float dx, dy;
        angleToDir(a, dx, dy);
        float x0 = cx + dx * kRadius * kSubInner;
        float y0 = cy + dy * kRadius * kSubInner;
        float x1 = cx + dx * kRadius * kSubOuter;
        float y1 = cy + dy * kRadius * kSubOuter;
        dl->AddLine(ImVec2(x0, y0), ImVec2(x1, y1),
                    IM_COL32(140, 140, 140, 160), 1.0f);
    }

    // Draw cardinal ticks + labels
    for (const auto& c : cardinals) {
        float a = c.baseAngle + rotation;
        float dx, dy;
        angleToDir(a, dx, dy);

        // Tick line
        float x0 = cx + dx * kRadius * kTickInner;
        float y0 = cy + dy * kRadius * kTickInner;
        float x1 = cx + dx * kRadius * kTickOuter;
        float y1 = cy + dy * kRadius * kTickOuter;

        bool isNorth = (c.baseAngle == 0.0f);
        float thickness = isNorth ? 3.0f : 2.0f;
        dl->AddLine(ImVec2(x0, y0), ImVec2(x1, y1), c.color, thickness);

        // Label
        float lx = cx + dx * kRadius * kLabelR;
        float ly = cy + dy * kRadius * kLabelR;

        // Center text on the label point
        ImVec2 textSize = ImGui::CalcTextSize(c.label);
        dl->AddText(ImVec2(lx - textSize.x * 0.5f, ly - textSize.y * 0.5f),
                    c.color, c.label);
    }

    // North triangle (small red arrow pointing toward N)
    {
        float a = 0.0f + rotation; // N direction
        float dx, dy;
        angleToDir(a, dx, dy);

        // Triangle tip at outer edge of inner area
        float tipX = cx + dx * kRadius * kTickInner;
        float tipY = cy + dy * kRadius * kTickInner;

        // Triangle base perpendicular to direction
        float perpDx = -dy;
        float perpDy = dx;
        float baseR = kRadius * kNorthTriR;
        float baseCx = cx + dx * kRadius * 0.15f;
        float baseCy = cy + dy * kRadius * 0.15f;

        ImVec2 tri[3] = {
            ImVec2(tipX, tipY),
            ImVec2(baseCx - perpDx * baseR * 0.4f, baseCy - perpDy * baseR * 0.4f),
            ImVec2(baseCx + perpDx * baseR * 0.4f, baseCy + perpDy * baseR * 0.4f),
        };
        dl->AddTriangleFilled(tri[0], tri[1], tri[2], IM_COL32(255, 80, 80, 180));
    }

    // Small center dot
    dl->AddCircleFilled(ImVec2(cx, cy), 2.5f, IM_COL32(200, 200, 200, 180));
}

} // namespace mapedit
