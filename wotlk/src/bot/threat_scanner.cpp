#include "threat_scanner.h"
#include "aggro.h"

#include <glog/logging.h>

#include <algorithm>
#include <cmath>
#include <set>

namespace bot {
namespace ThreatScanner {

// ---- Geometry helpers ----

bool SegmentIntersectsCircle2D(
    const game::Vec3& A, const game::Vec3& B,
    const game::Vec3& C, float R)
{
    // Parametric: P(t) = A + t*(B-A), t in [0,1]
    // |P(t) - C|^2 < R^2
    // Expand: a*t^2 + b*t + c < 0  where
    //   D = B - A,  F = A - C
    //   a = dot(D,D), b = 2*dot(F,D), c = dot(F,F) - R^2
    float dx = B.x - A.x;
    float dy = B.y - A.y;
    float fx = A.x - C.x;
    float fy = A.y - C.y;

    float a = dx * dx + dy * dy;
    float b = 2.0f * (fx * dx + fy * dy);
    float c = fx * fx + fy * fy - R * R;

    // If segment is a point, just check distance
    if (a < 1e-6f)
        return c < 0.0f;

    float discriminant = b * b - 4.0f * a * c;
    if (discriminant < 0.0f)
        return false;

    float sqrtDisc = std::sqrtf(discriminant);
    float t1 = (-b - sqrtDisc) / (2.0f * a);
    float t2 = (-b + sqrtDisc) / (2.0f * a);

    // Check if any t in [0,1] is a solution
    // The segment intersects if [t1,t2] overlaps [0,1]
    return t1 <= 1.0f && t2 >= 0.0f;
}

// ---- Threat detection ----

std::vector<BlockingThreat> GetBlockingThreats(
    const std::vector<game::Vec3>& path, size_t fromIndex)
{
    std::vector<BlockingThreat> result;

    if (path.size() < 2 || fromIndex + 1 >= path.size())
        return result;

    auto& radar = RadarData::Instance();
    const auto& entries = radar.GetEntries();

    // Track which GUIDs we've already added (NPC may block multiple segments)
    std::set<game::GUID> seen;

    for (size_t i = fromIndex; i + 1 < path.size(); ++i) {
        const game::Vec3& segA = path[i];
        const game::Vec3& segB = path[i + 1];

        for (const auto& e : entries) {
            // Filter: hostile NPCs only, alive, not already in combat, has aggro radius
            if (e.isPlayer) continue;
            if (e.isDead) continue;
            if (e.isInCombat) continue;
            if (e.aggroRadiusBuffered <= 0.0f) continue;
            if (e.reaction != game::UnitReaction::Hostile &&
                e.reaction != game::UnitReaction::Unfriendly)
                continue;

            if (seen.count(e.guid))
                continue;

            // Use buffered radius (R*1.15+3.0) for path validation — matches pathfinder
            if (SegmentIntersectsCircle2D(segA, segB, e.position, e.aggroRadiusBuffered)) {
                BlockingThreat threat;
                threat.entry = e;
                threat.segmentIndex = i;
                result.push_back(std::move(threat));
                seen.insert(e.guid);
            }
        }
    }

    return result;
}

const BlockingThreat* GetWeakestBlockingThreat(
    const std::vector<BlockingThreat>& threats)
{
    if (threats.empty())
        return nullptr;

    const BlockingThreat* weakest = &threats[0];
    for (size_t i = 1; i < threats.size(); ++i) {
        const auto& t = threats[i];
        if (t.entry.level < weakest->entry.level ||
            (t.entry.level == weakest->entry.level &&
             t.entry.distToPlayer < weakest->entry.distToPlayer))
        {
            weakest = &t;
        }
    }
    return weakest;
}

} // namespace ThreatScanner
} // namespace bot
