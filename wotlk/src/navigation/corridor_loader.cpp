#include "corridor_loader.h"
#include "nav_mesh.h"

#include <glog/logging.h>

#include <cmath>
#include <cstdlib>

namespace nav {

void CorridorLoader::PlanCorridor(const game::Vec3& from, const game::Vec3& to) {
    m_corridorTiles.clear();

    int tx0, ty0, tx1, ty1;
    NavMesh::WorldToTile(from.x, from.y, tx0, ty0);
    NavMesh::WorldToTile(to.x,   to.y,   tx1, ty1);

    // Rasterize the line between start and end tiles
    auto lineTiles = RasterizeLine(tx0, ty0, tx1, ty1);

    // Add +-1 lateral tiles for avoidance detours
    for (const auto& [tx, ty] : lineTiles) {
        for (int dx = -1; dx <= 1; ++dx) {
            for (int dy = -1; dy <= 1; ++dy) {
                m_corridorTiles.insert({tx + dx, ty + dy});
            }
        }
    }

    LOG(INFO) << "[CorridorLoader] Planned corridor from tile ("
              << tx0 << "," << ty0 << ") to (" << tx1 << "," << ty1
              << "): " << m_corridorTiles.size() << " tiles ("
              << lineTiles.size() << " line tiles)";
}

int CorridorLoader::LoadCorridor() {
    auto& nm = NavMesh::Instance();
    if (!nm.IsReady()) return 0;

    int loaded = 0;
    for (const auto& [tx, ty] : m_corridorTiles) {
        if (!nm.IsTileLoaded(tx, ty)) {
            // LoadTile is private — use UpdateLoadedTiles indirectly.
            // For direct loading, we need NavMesh to expose LoadTile or
            // accept a set of tiles to load. For now, we check if already loaded.
            // The actual loading happens via NavMesh::LoadTilesForCorridor.
        }
    }

    // Use the public tile loading API
    loaded = nm.LoadTiles(m_corridorTiles);

    if (loaded > 0) {
        LOG(INFO) << "[CorridorLoader] Loaded " << loaded << " corridor tiles ("
                  << nm.GetLoadedTileCount() << " total)";
    }
    return loaded;
}

void CorridorLoader::UnloadBehind(float playerX, float playerY, int keepRadius) {
    auto& nm = NavMesh::Instance();
    if (!nm.IsReady()) return;

    int playerTileX, playerTileY;
    NavMesh::WorldToTile(playerX, playerY, playerTileX, playerTileY);

    std::vector<std::pair<int,int>> toRemove;
    for (const auto& tile : m_corridorTiles) {
        int dx = std::abs(tile.first  - playerTileX);
        int dy = std::abs(tile.second - playerTileY);

        // If this corridor tile is far behind the player, remove it from our set
        // (NavMesh::UpdateLoadedTiles will handle the actual unloading based on
        // its own kTileUnloadRadius)
        if (dx > keepRadius + 2 || dy > keepRadius + 2) {
            toRemove.push_back(tile);
        }
    }

    for (const auto& t : toRemove)
        m_corridorTiles.erase(t);
}

void CorridorLoader::Clear() {
    m_corridorTiles.clear();
}

std::vector<std::pair<int,int>> CorridorLoader::RasterizeLine(int x0, int y0, int x1, int y1) {
    std::vector<std::pair<int,int>> tiles;

    int dx = std::abs(x1 - x0);
    int dy = std::abs(y1 - y0);
    int sx = (x0 < x1) ? 1 : -1;
    int sy = (y0 < y1) ? 1 : -1;
    int err = dx - dy;

    while (true) {
        tiles.push_back({x0, y0});

        if (x0 == x1 && y0 == y1)
            break;

        int e2 = 2 * err;
        if (e2 > -dy) {
            err -= dy;
            x0 += sx;
        }
        if (e2 < dx) {
            err += dx;
            y0 += sy;
        }
    }

    return tiles;
}

} // namespace nav
