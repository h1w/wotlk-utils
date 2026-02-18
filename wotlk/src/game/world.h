#pragma once
// =============================================================================
// World -- zone info, game state, line-of-sight, camera.
// =============================================================================

#include "types.h"
#include <string>

namespace game::world {

// --- Zone / Map ---
std::string GetZoneText();
std::string GetSubZoneText();
uint32_t    GetZoneId();
uint32_t    GetMapId();

// --- Realm ---
std::string GetRealmName();

// --- Game state ---
bool IsInGame();
bool IsLoading();

// --- Line of sight ---
// Returns true if there is a clear line between start and end (no collision).
bool HasLineOfSight(const Vec3& start, const Vec3& end);

// --- Camera ---
Vec3 GetCameraPosition();

} // namespace game::world
