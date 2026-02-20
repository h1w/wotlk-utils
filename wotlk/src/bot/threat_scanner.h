#pragma once
// =============================================================================
// ThreatScanner — detects hostile NPCs blocking a nav path, computes detours.
//
// Stateless utilities for threat detection and avoidance waypoint generation.
// Uses RadarData::Instance() for NPC positions and aggro radii.
// =============================================================================

#include "radar.h"
#include "../game/types.h"

#include <vector>
#include <cstddef>

namespace bot {

struct BlockingThreat {
    RadarEntry  entry;          // NPC data (position, guid, level, aggroRadius, etc.)
    size_t      segmentIndex;   // which path segment [i]->[i+1] it blocks
};

namespace ThreatScanner {

// Check each segment of path[fromIndex..end] against hostile NPC aggro circles.
// Returns NPCs whose aggro zones intersect any segment.
std::vector<BlockingThreat> GetBlockingThreats(
    const std::vector<game::Vec3>& path, size_t fromIndex);

// Return the weakest (lowest level, then closest) blocking threat.
// Returns nullptr if threats is empty.
const BlockingThreat* GetWeakestBlockingThreat(
    const std::vector<BlockingThreat>& threats);

// Check segment-circle intersection in 2D (XY plane).
// Returns true if any point on segment A->B is within radius R of center C.
bool SegmentIntersectsCircle2D(
    const game::Vec3& A, const game::Vec3& B,
    const game::Vec3& C, float R);

} // namespace ThreatScanner
} // namespace bot
