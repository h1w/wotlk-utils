#pragma once
#include <vector>
#include <cstdint>

class dtNavMeshQuery;

namespace mapedit {

class MapPathfinder;

class PlayerMarker {
public:
    // Current position in WoW world coords
    float posX = 0.0f, posY = 0.0f, posZ = 0.0f;
    float facing = 0.0f; // radians (WoW convention: 0=south, atan2(dy,dx))

    // Movement speed (yards per second)
    float speed = 7.0f; // WoW default run speed
    float speedMultiplier = 1.0f; // UI slider: 1x, 2x, 5x, 10x

    // Is the player placed on the map?
    bool placed = false;

    // Pathfinding route to follow
    struct RoutePoint { float x, y, z; };
    std::vector<RoutePoint> route;
    int currentWaypointIndex = 0;
    bool isMoving = false;
    bool loop = false;

    // Set a pathfinding destination (uses Detour to compute route)
    void SetDestination(float destX, float destY, float destZ,
                        MapPathfinder& pf, dtNavMeshQuery* query);

    // Follow an existing route (from RouteData)
    void FollowRoute(const std::vector<RoutePoint>& waypoints, bool looped);

    // Update position each frame (advance along route)
    void Update(float deltaTime);

    // Stop movement
    void Stop();

    // Is the player currently on a route?
    bool IsMoving() const { return isMoving; }

    // Place player at a position (click to teleport)
    void SetPosition(float x, float y, float z);

    // Clear placement
    void Clear();

private:
    // Internal update with recursion depth cap
    void AdvanceAlongRoute(float deltaTime, int depth);
};

} // namespace mapedit
