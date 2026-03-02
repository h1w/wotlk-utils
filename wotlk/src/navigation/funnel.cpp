#include "funnel.h"

#include <DetourNavMesh.h>
#include <DetourCommon.h>

#include <cmath>
#include <vector>

namespace nav {

// ---------------------------------------------------------------------------
// GetPortalPoints — reimplements Detour's private getPortalPoints using
// public dtNavMesh API (getTileAndPolyByRef, struct field access).
// Handles: internal edges, cross-tile boundaries, off-mesh connections.
// ---------------------------------------------------------------------------
static bool GetPortalPoints(const dtNavMesh* mesh,
                            dtPolyRef fromRef, dtPolyRef toRef,
                            float* left, float* right, bool& isOffMesh)
{
    isOffMesh = false;

    const dtMeshTile* fromTile = nullptr;
    const dtPoly* fromPoly = nullptr;
    if (dtStatusFailed(mesh->getTileAndPolyByRef(fromRef, &fromTile, &fromPoly)))
        return false;

    const dtMeshTile* toTile = nullptr;
    const dtPoly* toPoly = nullptr;
    if (dtStatusFailed(mesh->getTileAndPolyByRef(toRef, &toTile, &toPoly)))
        return false;

    // Off-mesh connection (from side): left=right=single vertex
    if (fromPoly->getType() == DT_POLYTYPE_OFFMESH_CONNECTION) {
        isOffMesh = true;
        for (unsigned int i = fromPoly->firstLink; i != DT_NULL_LINK; i = fromTile->links[i].next) {
            if (fromTile->links[i].ref == toRef) {
                const int v = fromTile->links[i].edge;
                dtVcopy(left, &fromTile->verts[fromPoly->verts[v] * 3]);
                dtVcopy(right, left);
                return true;
            }
        }
        return false;
    }

    // Off-mesh connection (to side): left=right=single vertex
    if (toPoly->getType() == DT_POLYTYPE_OFFMESH_CONNECTION) {
        isOffMesh = true;
        for (unsigned int i = toPoly->firstLink; i != DT_NULL_LINK; i = toTile->links[i].next) {
            if (toTile->links[i].ref == fromRef) {
                const int v = toTile->links[i].edge;
                dtVcopy(left, &toTile->verts[toPoly->verts[v] * 3]);
                dtVcopy(right, left);
                return true;
            }
        }
        return false;
    }

    // Find link from fromPoly -> toRef
    const dtLink* link = nullptr;
    for (unsigned int i = fromPoly->firstLink; i != DT_NULL_LINK; i = fromTile->links[i].next) {
        if (fromTile->links[i].ref == toRef) {
            link = &fromTile->links[i];
            break;
        }
    }
    if (!link)
        return false;

    // Portal vertices from the shared edge
    const int v0 = fromPoly->verts[link->edge];
    const int v1 = fromPoly->verts[(link->edge + 1) % fromPoly->vertCount];
    dtVcopy(left, &fromTile->verts[v0 * 3]);
    dtVcopy(right, &fromTile->verts[v1 * 3]);

    // Cross-tile boundary: clamp portal using link sub-edge limits
    if (link->side != 0xff) {
        if (link->bmin != 0 || link->bmax != 255) {
            const float s = 1.0f / 255.0f;
            const float tmin = link->bmin * s;
            const float tmax = link->bmax * s;
            dtVlerp(left, &fromTile->verts[v0 * 3], &fromTile->verts[v1 * 3], tmin);
            dtVlerp(right, &fromTile->verts[v0 * 3], &fromTile->verts[v1 * 3], tmax);
        }
    }

    return true;
}

// ---------------------------------------------------------------------------
// ShrinkPortal — contracts portal endpoints inward by margin yards (XZ plane).
// Clamps to midpoint if portal is narrower than 2*margin. Skips off-mesh.
// ---------------------------------------------------------------------------
static void ShrinkPortal(float* left, float* right, float margin, bool isOffMesh)
{
    if (isOffMesh || margin <= 0.0f)
        return;

    const float dx = right[0] - left[0];
    const float dy = right[1] - left[1];
    const float dz = right[2] - left[2];
    const float lenXZ = sqrtf(dx * dx + dz * dz);

    if (lenXZ < 1e-4f)
        return;

    // Narrow portal: collapse to midpoint. Forces the funnel to emit a waypoint
    // at the center of tight passages (doorframes, narrow corridors), preventing
    // the character from walking a straight line that clips the edges.
    // Threshold: 3*margin — with default 1.0yd, portals <= 3yd get collapsed.
    if (lenXZ <= margin * 3.0f) {
        const float mid0 = (left[0] + right[0]) * 0.5f;
        const float mid1 = (left[1] + right[1]) * 0.5f;
        const float mid2 = (left[2] + right[2]) * 0.5f;
        left[0] = right[0] = mid0;
        left[1] = right[1] = mid1;
        left[2] = right[2] = mid2;
        return;
    }

    // Normal shrink — margin is guaranteed < lenXZ * 0.5 (since lenXZ > 3*margin)
    const float t = margin / lenXZ;

    left[0]  += dx * t;
    left[1]  += dy * t;
    left[2]  += dz * t;

    right[0] -= dx * t;
    right[1] -= dy * t;
    right[2] -= dz * t;
}

// ---------------------------------------------------------------------------
// FunnelStraightPath — extract portals, shrink, run Simple Stupid Funnel.
// ---------------------------------------------------------------------------
bool FunnelStraightPath(const dtNavMesh* mesh,
                        const float* startPos,
                        const float* endPos,
                        const dtPolyRef* pathPolys,
                        int pathCount,
                        float margin,
                        float* outPoints,
                        int& outCount,
                        int maxPoints)
{
    outCount = 0;

    if (!mesh || pathCount < 1 || maxPoints < 2)
        return false;

    // Trivial: single polygon — straight line from start to end
    if (pathCount == 1) {
        dtVcopy(&outPoints[0], startPos);
        dtVcopy(&outPoints[3], endPos);
        outCount = 2;
        return true;
    }

    // --- Extract and shrink portals ---
    // Portal 0 = start (degenerate), portals 1..pathCount-1 = inter-poly,
    // portal pathCount = end (degenerate).
    const int portalCapacity = pathCount + 1;
    std::vector<Portal> portals(portalCapacity);
    int nPortals = 0;

    // Start portal (degenerate: left=right=startPos)
    dtVcopy(portals[0].left, startPos);
    dtVcopy(portals[0].right, startPos);
    portals[0].isOffMesh = false;
    portals[0].polyRef = pathPolys[0];
    nPortals = 1;

    for (int i = 0; i < pathCount - 1; ++i) {
        Portal& p = portals[nPortals];
        if (!GetPortalPoints(mesh, pathPolys[i], pathPolys[i + 1], p.left, p.right, p.isOffMesh))
            return false;

        ShrinkPortal(p.left, p.right, margin, p.isOffMesh);
        p.polyRef = pathPolys[i + 1];
        ++nPortals;
    }

    // End portal (degenerate: left=right=endPos)
    dtVcopy(portals[nPortals].left, endPos);
    dtVcopy(portals[nPortals].right, endPos);
    portals[nPortals].isOffMesh = false;
    portals[nPortals].polyRef = pathPolys[pathCount - 1];
    ++nPortals;

    // --- Simple Stupid Funnel Algorithm ---
    float apex[3], funnelLeft[3], funnelRight[3];
    dtVcopy(apex, startPos);
    dtVcopy(funnelLeft, startPos);
    dtVcopy(funnelRight, startPos);
    int apexIdx = 0, leftIdx = 0, rightIdx = 0;

    // Emit start point
    dtVcopy(&outPoints[outCount * 3], apex);
    ++outCount;

    for (int i = 1; i < nPortals; ++i) {
        const float* pLeft = portals[i].left;
        const float* pRight = portals[i].right;

        // Update right side of funnel
        if (dtTriArea2D(apex, funnelRight, pRight) <= 0.0f) {
            if (dtVdistSqr(apex, funnelRight) < 1e-6f ||
                dtTriArea2D(apex, funnelLeft, pRight) > 0.0f) {
                // Tighten the funnel
                dtVcopy(funnelRight, pRight);
                rightIdx = i;
            } else {
                // Right crossed left — emit left vertex as new waypoint
                if (outCount >= maxPoints)
                    return true;
                dtVcopy(&outPoints[outCount * 3], funnelLeft);
                ++outCount;

                // Reset funnel from left
                dtVcopy(apex, funnelLeft);
                apexIdx = leftIdx;
                dtVcopy(funnelLeft, apex);
                dtVcopy(funnelRight, apex);
                leftIdx = apexIdx;
                rightIdx = apexIdx;
                i = apexIdx;  // loop increment -> apexIdx + 1
                continue;
            }
        }

        // Update left side of funnel
        if (dtTriArea2D(apex, funnelLeft, pLeft) >= 0.0f) {
            if (dtVdistSqr(apex, funnelLeft) < 1e-6f ||
                dtTriArea2D(apex, funnelRight, pLeft) < 0.0f) {
                // Tighten the funnel
                dtVcopy(funnelLeft, pLeft);
                leftIdx = i;
            } else {
                // Left crossed right — emit right vertex as new waypoint
                if (outCount >= maxPoints)
                    return true;
                dtVcopy(&outPoints[outCount * 3], funnelRight);
                ++outCount;

                // Reset funnel from right
                dtVcopy(apex, funnelRight);
                apexIdx = rightIdx;
                dtVcopy(funnelLeft, apex);
                dtVcopy(funnelRight, apex);
                leftIdx = apexIdx;
                rightIdx = apexIdx;
                i = apexIdx;  // loop increment -> apexIdx + 1
                continue;
            }
        }
    }

    // Emit end point
    if (outCount < maxPoints) {
        dtVcopy(&outPoints[outCount * 3], endPos);
        ++outCount;
    }

    return outCount >= 2;
}

} // namespace nav
