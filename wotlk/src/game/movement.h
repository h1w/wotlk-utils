#pragma once
// =============================================================================
// Movement -- ClickToMove (C++), facing, movement keys, Jump (Lua).
// =============================================================================

#include "types.h"
#include <cstdint>

namespace game::movement {

// --- ClickToMove (C++) ---
bool ClickToMove(const Vec3& pos);
bool ClickToMoveAttack(GUID targetGuid, const Vec3& pos);
bool ClickToMoveInteract(GUID targetGuid, const Vec3& pos);
bool ClickToMoveLoot(GUID targetGuid, const Vec3& pos);
bool StopCTM();

// --- Facing (C++) ---
bool SetFacing(float radians);
bool FacePosition(const Vec3& target);

// --- Lua movement ---
bool Jump();
bool StopMoving();

} // namespace game::movement
