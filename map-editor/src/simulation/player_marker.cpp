#include "player_marker.h"
#include "../navmesh/pathfinder.h"
#include <cmath>
#include <algorithm>

namespace mapedit {

void PlayerMarker::SetDestination(float destX, float destY, float destZ,
                                  MapPathfinder& pf, dtNavMeshQuery* query)
{
    PathResult result = pf.FindPath(query, posX, posY, posZ, destX, destY, destZ);
    if (!result.success && !result.partial)
        return;

    route.clear();
    route.reserve(result.waypoints.size());
    for (const auto& wp : result.waypoints) {
        route.push_back({ wp.x, wp.y, wp.z });
    }

    currentWaypointIndex = 0;
    isMoving = !route.empty();
    loop = false;
}

void PlayerMarker::FollowRoute(const std::vector<RoutePoint>& waypoints, bool looped)
{
    route = waypoints;
    currentWaypointIndex = 0;
    loop = looped;
    isMoving = !route.empty();
}

void PlayerMarker::Update(float deltaTime)
{
    if (!isMoving || route.empty())
        return;

    AdvanceAlongRoute(deltaTime, 0);
}

void PlayerMarker::AdvanceAlongRoute(float deltaTime, int depth)
{
    // Cap recursion to prevent infinite loops (e.g. zero-length segments)
    if (depth >= 3)
        return;

    if (!isMoving || route.empty())
        return;

    if (currentWaypointIndex >= static_cast<int>(route.size())) {
        if (loop) {
            currentWaypointIndex = 0;
        } else {
            isMoving = false;
            return;
        }
    }

    const RoutePoint& target = route[currentWaypointIndex];
    float dx = target.x - posX;
    float dy = target.y - posY;
    float dz = target.z - posZ;
    float dist = sqrtf(dx * dx + dy * dy + dz * dz);

    float effectiveSpeed = speed * speedMultiplier;
    float step = effectiveSpeed * deltaTime;

    if (dist < 1e-6f) {
        // Already at waypoint, advance to next
        currentWaypointIndex++;
        AdvanceAlongRoute(deltaTime, depth + 1);
        return;
    }

    // Update facing toward target (WoW convention: atan2(dy, dx))
    facing = atan2f(dy, dx);

    if (step >= dist) {
        // Arrived at waypoint
        posX = target.x;
        posY = target.y;
        posZ = target.z;

        float remainingTime = deltaTime - (dist / effectiveSpeed);
        remainingTime = (std::max)(0.0f, remainingTime);

        currentWaypointIndex++;

        if (currentWaypointIndex >= static_cast<int>(route.size())) {
            if (loop) {
                currentWaypointIndex = 0;
                AdvanceAlongRoute(remainingTime, depth + 1);
            } else {
                isMoving = false;
            }
        } else {
            AdvanceAlongRoute(remainingTime, depth + 1);
        }
    } else {
        // Move toward waypoint
        float t = step / dist;
        posX += dx * t;
        posY += dy * t;
        posZ += dz * t;
    }
}

void PlayerMarker::Stop()
{
    isMoving = false;
}

void PlayerMarker::SetPosition(float x, float y, float z)
{
    posX = x;
    posY = y;
    posZ = z;
    placed = true;
    Stop();
}

void PlayerMarker::Clear()
{
    placed = false;
    Stop();
}

} // namespace mapedit
