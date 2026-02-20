#pragma once
// =============================================================================
// CorridorLoader — pre-loads navmesh tiles along a long Walk edge.
//
// When the bot needs to walk between two distant WorldNodes, the 5x5 tile grid
// around the player isn't enough to plan the full route. CorridorLoader
// computes the set of tiles along the corridor and loads them in advance.
// =============================================================================

#include "../game/types.h"

#include <cstdint>
#include <set>
#include <utility>
#include <vector>

namespace nav {

class CorridorLoader {
public:
    // Compute the corridor tiles between two world positions (same map).
    // Includes +-1 tile laterally for avoidance detours.
    // Does NOT load them — call LoadCorridor() next.
    void PlanCorridor(const game::Vec3& from, const game::Vec3& to);

    // Load all planned corridor tiles that aren't already loaded.
    // Returns number of newly loaded tiles.
    int LoadCorridor();

    // Unload corridor tiles that are behind the player (already traversed).
    // Keeps tiles within keepRadius of the player's tile.
    void UnloadBehind(float playerX, float playerY, int keepRadius = 2);

    // Clear the corridor plan (call when walk segment completes).
    void Clear();

    bool HasCorridor() const { return !m_corridorTiles.empty(); }
    int  GetCorridorTileCount() const { return static_cast<int>(m_corridorTiles.size()); }

    // Get all corridor tiles for debug/UI.
    const std::set<std::pair<int,int>>& GetCorridorTiles() const { return m_corridorTiles; }

private:
    std::set<std::pair<int,int>> m_corridorTiles;

    // Bresenham-like line rasterization in tile space.
    static std::vector<std::pair<int,int>> RasterizeLine(int x0, int y0, int x1, int y1);
};

} // namespace nav
