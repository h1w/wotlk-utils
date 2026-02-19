#pragma once
// =============================================================================
// Game SDK facade -- high-level API for reading game state and performing
// actions. Initialize() / Shutdown() from dllmain.cpp.
// =============================================================================

#include "types.h"
#include "local_player.h"
#include "unit.h"
#include "game_object.h"

#include <optional>
#include <vector>

namespace game {

// --- Lifecycle ---
bool Initialize();
void Shutdown();

// --- Player ---
std::optional<LocalPlayer> GetLocalPlayer();
std::optional<Unit>        GetTarget();
std::optional<Unit>        GetMouseOver();

// --- Object enumeration ---
std::vector<Unit>      GetAllUnits();
std::vector<Unit>      GetUnitsInRange(float maxDist);
std::vector<WowObject> GetAllGameObjects();

// --- Targeting ---
bool SelectTarget(GUID guid);

} // namespace game
