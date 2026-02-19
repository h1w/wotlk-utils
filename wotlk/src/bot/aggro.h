#pragma once
// =============================================================================
// Aggro radius formula -- shared between radar widget and nav avoidance.
// =============================================================================

#include <algorithm>

namespace bot {

// Standard WoW aggro radius: 20 yards +/- level difference, clamped [5, 45].
inline float CalcAggroRadius(int creatureLevel, int playerLevel) {
    float r = 20.0f + static_cast<float>(creatureLevel - playerLevel);
    return std::clamp(r, 5.0f, 45.0f);
}

// Safety margin added to aggro radius for avoidance / visualization.
inline constexpr float kAggroMargin = 3.0f;

} // namespace bot
