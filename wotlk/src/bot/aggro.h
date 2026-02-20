#pragma once
// =============================================================================
// Aggro radius formula -- shared between radar widget and nav avoidance.
// =============================================================================

#include <algorithm>

namespace bot {

// Standard WoW aggro radius: 20 yards +/- level difference, clamped [5, 45].
// Used for radar display (shows real aggro ring).
inline float CalcAggroRadius(int creatureLevel, int playerLevel) {
    float r = 20.0f + static_cast<float>(creatureLevel - playerLevel);
    return std::clamp(r, 5.0f, 45.0f);
}

// Buffered aggro radius for visualization (radar ring) and path validation.
// Matches Pathfinder::kAggroMarginMult / kAggroMarginAdd.
inline float CalcAggroRadiusBuffered(int creatureLevel, int playerLevel) {
    float raw = CalcAggroRadius(creatureLevel, playerLevel);
    return raw * 1.15f + 3.0f;
}

// Navigation aggro radius: base 30 yards +/- level difference, clamped [5, 55].
// Wider than real aggro to give navigation extra safety margin around hostiles.
// Pathfinder applies its own margin on top (R * 1.15 + 3.0).
inline float CalcAggroRadiusNav(int creatureLevel, int playerLevel) {
    float r = 30.0f + static_cast<float>(creatureLevel - playerLevel);
    return std::clamp(r, 5.0f, 55.0f);
}

} // namespace bot
