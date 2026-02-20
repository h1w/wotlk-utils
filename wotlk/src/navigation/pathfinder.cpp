#include "pathfinder.h"
#include "nav_mesh.h"

#include <DetourNavMesh.h>
#include <DetourNavMeshQuery.h>
#include <DetourStatus.h>
#include <DetourCommon.h>

#include <glog/logging.h>

#include <cstdint>
#include <cmath>

namespace nav {

// File-scope constant — mirrors Pathfinder::kDangerAreaId for use in PolyAreaGuard.
static constexpr uint8_t kDangerArea = 63;

// RAII guard to restore poly areas after danger marking.
// Only saves/restores areas — never touches poly flags.
struct PolyAreaGuard {
    dtNavMesh* mesh = nullptr;
    struct Saved { dtPolyRef ref; uint8_t area; };
    std::vector<Saved> saved;

    // Mark a poly as danger (area 63). Saves original area for restoration.
    void MarkDanger(dtPolyRef ref) {
        uint8_t origArea = 0;
        if (dtStatusFailed(mesh->getPolyArea(ref, &origArea)))
            return;
        if (origArea == kDangerArea)
            return;  // already marked by another zone
        saved.push_back({ ref, origArea });
        mesh->setPolyArea(ref, kDangerArea);
    }

    ~PolyAreaGuard() {
        if (!mesh) return;
        for (auto& s : saved)
            mesh->setPolyArea(s.ref, s.area);
    }
};


Pathfinder& Pathfinder::Instance() {
    static Pathfinder s_instance;
    return s_instance;
}

Pathfinder::Pathfinder() {
    ResetAreaCosts();
}

void Pathfinder::ResetAreaCosts() {
    for (int i = 0; i < 64; ++i)
        m_areaCosts[i] = 1.0f;
}

void Pathfinder::SetAreaCost(uint8_t areaId, float cost) {
    if (areaId < 64)
        m_areaCosts[areaId] = cost;
}

void Pathfinder::BuildFilter(dtQueryFilter& filter) const {
    filter.setIncludeFlags(0xFFFF);
    filter.setExcludeFlags(0);
    for (int i = 0; i < 64; ++i)
        filter.setAreaCost(i, m_areaCosts[i]);
}

// Core pathfinding with a caller-provided filter
PathResult Pathfinder::FindPathWithFilter(
    const game::Vec3& start, const game::Vec3& end,
    const dtQueryFilter& filter)
{
    PathResult result;

    auto& navMesh = NavMesh::Instance();
    dtNavMeshQuery* query = navMesh.GetQuery();
    if (!query) return result;

    // TrinityCore mmaps: Detour[0]=WowY, Detour[1]=WowZ (up), Detour[2]=WowX
    float startPos[3] = { start.y, start.z, start.x };
    float endPos[3]   = { end.y,   end.z,   end.x };
    float extents[3]  = { kSearchExtentXZ, kSearchExtentY, kSearchExtentXZ };

    // Find nearest polygons
    dtPolyRef startRef = 0, endRef = 0;
    float closestStart[3], closestEnd[3];

    dtStatus status = query->findNearestPoly(startPos, extents, &filter, &startRef, closestStart);
    if (dtStatusFailed(status) || startRef == 0) {
        int tileX, tileY;
        navMesh.WorldToTile(start.x, start.y, tileX, tileY);
        LOG(WARNING) << "[Nav] FindPath: no start poly near ("
                     << start.x << ", " << start.y << ", " << start.z
                     << ") tile=(" << tileX << "," << tileY
                     << ") loaded=" << navMesh.IsTileLoaded(tileX, tileY)
                     << " totalTiles=" << navMesh.GetLoadedTileCount()
                     << " dtStatus=0x" << std::hex << status << std::dec;
        return result;
    }

    status = query->findNearestPoly(endPos, extents, &filter, &endRef, closestEnd);
    if (dtStatusFailed(status) || endRef == 0) {
        // Retry with expanded extents
        float largeExtents[3] = { kLargeExtentXZ, kLargeExtentY, kLargeExtentXZ };
        status = query->findNearestPoly(endPos, largeExtents, &filter, &endRef, closestEnd);

        if (dtStatusFailed(status) || endRef == 0) {
            int tileX, tileY;
            navMesh.WorldToTile(end.x, end.y, tileX, tileY);
            LOG(WARNING) << "[Nav] FindPath: no end poly near ("
                         << end.x << ", " << end.y << ", " << end.z
                         << ") tile=(" << tileX << "," << tileY
                         << ") loaded=" << navMesh.IsTileLoaded(tileX, tileY)
                         << " totalTiles=" << navMesh.GetLoadedTileCount()
                         << " dtStatus=0x" << std::hex << status << std::dec;
            return result;
        }

        LOG(INFO) << "[Nav] FindPath: end poly found with expanded extents";
    }

    // A* through polygon graph
    dtPolyRef pathPolys[kMaxPathPolygons];
    int pathCount = 0;

    status = query->findPath(startRef, endRef, closestStart, closestEnd,
                             &filter, pathPolys, &pathCount, kMaxPathPolygons);

    if (dtStatusFailed(status) || pathCount == 0) {
        LOG(WARNING) << "[Nav] FindPath: findPath failed, status=0x" << std::hex << status;
        return result;
    }

    if (dtStatusDetail(status, DT_PARTIAL_RESULT))
        result.partial = true;

    // String-pull to straight path
    float straightPath[kMaxStraightPath * 3];
    unsigned char straightPathFlags[kMaxStraightPath];
    dtPolyRef straightPathPolys[kMaxStraightPath];
    int straightPathCount = 0;

    status = query->findStraightPath(closestStart, closestEnd,
                                     pathPolys, pathCount,
                                     straightPath, straightPathFlags, straightPathPolys,
                                     &straightPathCount, kMaxStraightPath,
                                     DT_STRAIGHTPATH_ALL_CROSSINGS);

    if (dtStatusFailed(status) || straightPathCount == 0) {
        LOG(WARNING) << "[Nav] FindPath: findStraightPath failed";
        return result;
    }

    // Convert back: WoW X = Detour[2], WoW Y = Detour[0], WoW Z = Detour[1]
    result.waypoints.reserve(straightPathCount);
    for (int i = 0; i < straightPathCount; ++i) {
        game::Vec3 wp;
        wp.x = straightPath[i * 3 + 2];
        wp.y = straightPath[i * 3 + 0];
        wp.z = straightPath[i * 3 + 1] + kZOffset;
        result.waypoints.push_back(wp);
    }

    result.success = true;

    LOG(INFO) << "[Nav] Path found: " << result.waypoints.size() << " waypoints"
              << (result.partial ? " (PARTIAL)" : "");

    return result;
}

PathResult Pathfinder::FindPath(const game::Vec3& start, const game::Vec3& end) {
    auto& navMesh = NavMesh::Instance();
    if (!navMesh.IsReady()) {
        LOG(WARNING) << "[Nav] FindPath: navmesh not ready";
        return PathResult{};
    }

    dtQueryFilter filter;
    BuildFilter(filter);
    return FindPathWithFilter(start, end, filter);
}

// ---------------------------------------------------------------------------
// FindPathAvoiding — single-pass area cost + mandatory post-validation.
//
// 1. Mark danger polys with area 63 (kDangerAreaId). PolyAreaGuard restores
//    original areas on scope exit (RAII). NEVER touches poly flags.
// 2. Use permissive filter (all costs=1.0) for findNearestPoly — guarantees
//    we always find start/end polys even inside danger zones.
// 3. Use danger-aware filter (area 63 = cost 50x) for findPath — A* steers
//    around danger but CAN enter if no safe path exists.
// 4. Post-validate: check intermediate polys for kDangerAreaId to determine
//    throughDanger. Skip first and last poly (player may start/end in danger).
// ---------------------------------------------------------------------------
PathResult Pathfinder::FindPathAvoiding(
    const game::Vec3& start, const game::Vec3& end,
    const std::vector<DangerZone>& dangers)
{
    if (dangers.empty())
        return FindPath(start, end);

    auto& navMesh = NavMesh::Instance();
    if (!navMesh.IsReady()) {
        LOG(WARNING) << "[Nav] FindPathAvoiding: navmesh not ready";
        return PathResult{};
    }

    dtNavMesh* mesh = navMesh.GetNavMesh();
    dtNavMeshQuery* query = navMesh.GetQuery();
    if (!mesh || !query)
        return PathResult{};

    // --- Permissive filter (for queryPolygons + findNearestPoly) ---
    // All polys pass, all costs = 1.0. Never excludes anything.
    dtQueryFilter permissiveFilter;
    BuildFilter(permissiveFilter);

    // --- Mark danger polygons (area 63) ---
    PolyAreaGuard guard;
    guard.mesh = mesh;

    dtPolyRef candidatePolys[kMaxDangerPolys];

    for (const auto& dz : dangers) {
        // Effective radius: raw aggro radius * 1.15 + 3.0
        float effectiveR = dz.radius * kAggroMarginMult + kAggroMarginAdd;

        // Detour coords: [WowY, WowZ, WowX]
        float center[3] = { dz.y, dz.z, dz.x };
        float halfExtents[3] = { effectiveR, kAggroQueryExtentY, effectiveR };
        float rSq = effectiveR * effectiveR;

        int polyCount = 0;
        dtStatus st = query->queryPolygons(center, halfExtents, &permissiveFilter,
                                           candidatePolys, &polyCount, kMaxDangerPolys);
        if (dtStatusFailed(st) || polyCount == 0)
            continue;

        for (int i = 0; i < polyCount; ++i) {
            dtPolyRef ref = candidatePolys[i];

            // Precise circle test: closest point on polygon to zone center
            float closestPt[3];
            bool posOverPoly;
            if (dtStatusFailed(query->closestPointOnPoly(ref, center, closestPt, &posOverPoly)))
                continue;

            // 2D distance (Detour XZ plane = WoW XY plane)
            float dx = closestPt[0] - center[0];
            float dzz = closestPt[2] - center[2];
            if (dx * dx + dzz * dzz > rSq)
                continue;  // outside circle

            guard.MarkDanger(ref);
        }
    }

    LOG(INFO) << "[Nav] FindPathAvoiding: " << guard.saved.size()
              << " polys marked in " << dangers.size() << " danger zones"
              << " (margin=" << kAggroMarginMult << "x+" << kAggroMarginAdd << "yd)"
              << " start=(" << start.x << "," << start.y
              << ") end=(" << end.x << "," << end.y << ")";

    if (guard.saved.empty()) {
        // No navmesh polys overlap with danger zones — normal path is safe
        return FindPath(start, end);
    }

    // --- Find start/end polys with PERMISSIVE filter ---
    // Critical: permissive filter guarantees findNearestPoly always works,
    // even if the player stands inside a danger zone.
    float startPos[3] = { start.y, start.z, start.x };
    float endPos[3]   = { end.y,   end.z,   end.x };
    float extents[3]  = { kSearchExtentXZ, kSearchExtentY, kSearchExtentXZ };

    dtPolyRef startRef = 0, endRef = 0;
    float closestStart[3], closestEnd[3];

    dtStatus status = query->findNearestPoly(startPos, extents, &permissiveFilter,
                                              &startRef, closestStart);
    if (dtStatusFailed(status) || startRef == 0) {
        int tileX, tileY;
        navMesh.WorldToTile(start.x, start.y, tileX, tileY);
        LOG(WARNING) << "[Nav] FindPathAvoiding: no start poly near ("
                     << start.x << ", " << start.y << ", " << start.z
                     << ") tile=(" << tileX << "," << tileY
                     << ") loaded=" << navMesh.IsTileLoaded(tileX, tileY);
        return PathResult{};
    }

    bool partialEnd = false;
    status = query->findNearestPoly(endPos, extents, &permissiveFilter, &endRef, closestEnd);
    if (dtStatusFailed(status) || endRef == 0) {
        float largeExtents[3] = { kLargeExtentXZ, kLargeExtentY, kLargeExtentXZ };
        status = query->findNearestPoly(endPos, largeExtents, &permissiveFilter,
                                         &endRef, closestEnd);
        if (dtStatusFailed(status) || endRef == 0) {
            int tileX, tileY;
            navMesh.WorldToTile(end.x, end.y, tileX, tileY);
            LOG(WARNING) << "[Nav] FindPathAvoiding: no end poly near ("
                         << end.x << ", " << end.y << ", " << end.z
                         << ") tile=(" << tileX << "," << tileY
                         << ") loaded=" << navMesh.IsTileLoaded(tileX, tileY);
            return PathResult{};
        }
        partialEnd = true;
        LOG(INFO) << "[Nav] FindPathAvoiding: end poly found with expanded extents (PARTIAL)";
    }

    // --- Danger-aware filter (area 63 = cost 50x) for findPath ---
    dtQueryFilter dangerFilter;
    BuildFilter(dangerFilter);
    dangerFilter.setAreaCost(kDangerAreaId, kDangerAreaCost);

    dtPolyRef pathPolys[kMaxPathPolygons];
    int pathCount = 0;

    status = query->findPath(startRef, endRef, closestStart, closestEnd,
                             &dangerFilter, pathPolys, &pathCount, kMaxPathPolygons);

    if (dtStatusFailed(status) || pathCount == 0) {
        LOG(WARNING) << "[Nav] FindPathAvoiding: findPath failed, status=0x"
                     << std::hex << status << std::dec;
        return PathResult{};
    }

    bool partial = partialEnd || dtStatusDetail(status, DT_PARTIAL_RESULT);

    // --- Post-validation: check intermediate polys against actual marked areas ---
    // Skip first poly (player may start in danger) and last poly (destination may be in danger).
    // Only intermediate polys indicate "path goes THROUGH danger".
    int dangerPolyCount = 0;
    bool throughDanger = false;
    for (int i = 0; i < pathCount; ++i) {
        uint8_t area = 0;
        mesh->getPolyArea(pathPolys[i], &area);
        if (area == kDangerAreaId) {
            ++dangerPolyCount;
            if (i > 0 && i < pathCount - 1)
                throughDanger = true;
        }
    }

    LOG(INFO) << "[Nav] FindPathAvoiding: " << pathCount << " polys ("
              << dangerPolyCount << " danger, " << (pathCount - dangerPolyCount) << " safe)"
              << (partial ? " PARTIAL" : "")
              << (throughDanger ? " THROUGH_DANGER" : " SAFE")
              << (dtStatusDetail(status, DT_OUT_OF_NODES) ? " OUT_OF_NODES" : "")
              << " status=0x" << std::hex << status << std::dec;

    auto result = BuildStraightPath(query, closestStart, closestEnd,
                                    pathPolys, pathCount, throughDanger, partial);

    // --- Geometric post-processing: nudge waypoints away from aggro circles ---
    // Poly-level marking can miss cases where string-pulled line segments clip aggro
    // circles even though the polys themselves are "safe" (polygon granularity issue).
    // Fix by pushing offending waypoints outward from the circle center.
    if (result.success && result.waypoints.size() >= 2) {
        if (NudgeWaypointsFromDangers(result, dangers, kAggroMarginMult, kAggroMarginAdd)) {
            LOG(INFO) << "[Nav] Nudged waypoints to avoid aggro circle clipping";
        }
    }

    return result;
}

// ---------------------------------------------------------------------------
// Helper: string-pull A* poly path into straight waypoints
// ---------------------------------------------------------------------------
PathResult Pathfinder::BuildStraightPath(
    dtNavMeshQuery* query,
    const float* closestStart, const float* closestEnd,
    const dtPolyRef* pathPolys, int pathCount,
    bool throughDanger, bool partial)
{
    PathResult result;

    float straightPath[kMaxStraightPath * 3];
    unsigned char straightPathFlags[kMaxStraightPath];
    dtPolyRef straightPathPolys[kMaxStraightPath];
    int straightPathCount = 0;

    dtStatus status = query->findStraightPath(
        closestStart, closestEnd,
        pathPolys, pathCount,
        straightPath, straightPathFlags, straightPathPolys,
        &straightPathCount, kMaxStraightPath,
        DT_STRAIGHTPATH_ALL_CROSSINGS);

    if (dtStatusFailed(status) || straightPathCount == 0) {
        LOG(WARNING) << "[Nav] BuildStraightPath: findStraightPath failed";
        return result;
    }

    result.waypoints.reserve(straightPathCount);
    for (int i = 0; i < straightPathCount; ++i) {
        game::Vec3 wp;
        wp.x = straightPath[i * 3 + 2];  // WoW X = Detour[2]
        wp.y = straightPath[i * 3 + 0];  // WoW Y = Detour[0]
        wp.z = straightPath[i * 3 + 1] + kZOffset;  // WoW Z = Detour[1]
        result.waypoints.push_back(wp);
    }

    result.success = true;
    result.partial = partial;
    result.throughDanger = throughDanger;

    LOG(INFO) << "[Nav] FindPathAvoiding: " << result.waypoints.size() << " waypoints"
              << (result.throughDanger ? " (THROUGH DANGER)" : " (safe)")
              << (result.partial ? " (PARTIAL)" : "");

    return result;
}

// ---------------------------------------------------------------------------
// NudgeWaypointsFromDangers — push waypoints away from aggro circles.
//
// For each segment that clips an aggro circle, inserts a waypoint at the
// closest approach point, pushed outward from the circle center by the
// effective radius + small buffer. This fixes visual clipping that poly-level
// marking misses due to coarse navmesh polygon granularity.
// ---------------------------------------------------------------------------
bool Pathfinder::NudgeWaypointsFromDangers(
    PathResult& result,
    const std::vector<DangerZone>& dangers,
    float marginMult, float marginAdd)
{
    static constexpr float kNudgeBuffer = 2.0f; // extra yards past effective radius
    static constexpr int   kMaxInserts  = 20;   // limit to prevent runaway

    bool anyNudged = false;
    int inserts = 0;

    for (size_t i = 0; i + 1 < result.waypoints.size() && inserts < kMaxInserts; ++i) {
        const auto& a = result.waypoints[i];
        const auto& b = result.waypoints[i + 1];

        for (const auto& dz : dangers) {
            float effectiveR = dz.radius * marginMult + marginAdd;
            float cx = dz.x, cy = dz.y;

            // 2D segment-circle intersection test
            float dx = b.x - a.x, dy = b.y - a.y;
            float fx = a.x - cx, fy = a.y - cy;
            float segLenSq = dx * dx + dy * dy;
            if (segLenSq < 0.001f) continue;

            float bb = 2.0f * (fx * dx + fy * dy);
            float cc = fx * fx + fy * fy - effectiveR * effectiveR;
            float disc = bb * bb - 4.0f * segLenSq * cc;
            if (disc < 0.0f) continue;

            float sqrtDisc = std::sqrt(disc);
            float t1 = (-bb - sqrtDisc) / (2.0f * segLenSq);
            float t2 = (-bb + sqrtDisc) / (2.0f * segLenSq);

            // Segment intersects if any t in [0,1]
            if (t1 > 1.0f || t2 < 0.0f) continue;

            // Find closest approach point on segment to circle center (clamped to [0,1])
            float tClosest = -(fx * dx + fy * dy) / segLenSq;
            tClosest = (tClosest < 0.0f) ? 0.0f : (tClosest > 1.0f) ? 1.0f : tClosest;

            float nearX = a.x + tClosest * dx;
            float nearY = a.y + tClosest * dy;
            float nearZ = a.z + tClosest * (b.z - a.z);

            // Push outward from circle center
            float pushDx = nearX - cx;
            float pushDy = nearY - cy;
            float pushDist = std::sqrt(pushDx * pushDx + pushDy * pushDy);
            if (pushDist < 0.001f) {
                // Degenerate: segment goes through circle center. Push perpendicular to segment.
                pushDx = -dy;
                pushDy = dx;
                pushDist = std::sqrt(pushDx * pushDx + pushDy * pushDy);
                if (pushDist < 0.001f) continue;
            }

            float pushR = effectiveR + kNudgeBuffer;
            float nudgedX = cx + (pushDx / pushDist) * pushR;
            float nudgedY = cy + (pushDy / pushDist) * pushR;

            // Insert the nudged waypoint between i and i+1
            game::Vec3 nudgedWP = { nudgedX, nudgedY, nearZ };
            result.waypoints.insert(result.waypoints.begin() + static_cast<int>(i + 1), nudgedWP);
            ++inserts;
            anyNudged = true;
            break;  // Re-check this segment pair (i, i+1) is now (i, nudged)
        }
    }

    return anyNudged;
}

} // namespace nav
