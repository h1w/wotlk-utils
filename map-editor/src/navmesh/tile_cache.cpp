#include "tile_cache.h"
#include "../canvas/canvas.h"

#include <DetourNavMesh.h>
#include <DetourNavMeshQuery.h>

#include <glog/logging.h>

#include <algorithm>
#include <cmath>
#include <set>
#include <vector>

namespace mapedit {

static constexpr float kTileSize = 533.33333f;

TileCache::~TileCache() {
    Clear();
}

bool TileCache::SetMap(const std::string& mmapDir, uint32_t mapId) {
    if (m_mapId == mapId && m_mesh)
        return true;

    Clear();

    m_mmapDir = mmapDir;
    m_mapId = mapId;

    m_mesh = CreateNavMesh(mmapDir, mapId);
    if (!m_mesh) {
        LOG(ERROR) << "[TileCache] Failed to create navmesh for map " << mapId;
        return false;
    }

    m_query = CreateNavMeshQuery(m_mesh);
    if (!m_query) {
        LOG(ERROR) << "[TileCache] Failed to create query for map " << mapId;
        dtFreeNavMesh(m_mesh);
        m_mesh = nullptr;
        return false;
    }

    return true;
}

void TileCache::UpdateViewport(const Canvas& canvas) {
    if (!m_mesh) return;

    // Center tile from canvas center
    // TC tile files: tile (gx,gy) covers wowX ∈ [(32-gx-1)*533.33, (32-gx)*533.33)
    // so: gx = 32 - floor(wowX/533.33) - 1  (= 31 - floor(wowX/533.33))
    int centerTX = 31 - static_cast<int>(std::floor(canvas.centerX / kTileSize));
    int centerTY = 31 - static_cast<int>(std::floor(canvas.centerY / kTileSize));

    // Viewport tile range
    float minX, maxX, minY, maxY;
    canvas.GetViewBounds(minX, maxX, minY, maxY);

    int tMinX = 31 - static_cast<int>(std::floor(maxX / kTileSize));
    int tMaxX = 31 - static_cast<int>(std::floor(minX / kTileSize));
    int tMinY = 31 - static_cast<int>(std::floor(maxY / kTileSize));
    int tMaxY = 31 - static_cast<int>(std::floor(minY / kTileSize));

    tMinX = std::clamp(tMinX, 0, 63);
    tMaxX = std::clamp(tMaxX, 0, 63);
    tMinY = std::clamp(tMinY, 0, 63);
    tMaxY = std::clamp(tMaxY, 0, 63);

    // Build list of viewport tiles sorted by distance from center (nearest first)
    struct TileDist {
        int tx, ty;
        int dist; // squared distance in tile coords
    };
    std::vector<TileDist> candidates;
    candidates.reserve((tMaxX - tMinX + 1) * (tMaxY - tMinY + 1));

    for (int tx = tMinX; tx <= tMaxX; ++tx) {
        for (int ty = tMinY; ty <= tMaxY; ++ty) {
            int dx = tx - centerTX;
            int dy = ty - centerTY;
            candidates.push_back({tx, ty, dx * dx + dy * dy});
        }
    }

    std::sort(candidates.begin(), candidates.end(),
              [](const TileDist& a, const TileDist& b) { return a.dist < b.dist; });

    // Take the first m_maxTiles — these are the wanted tiles
    if (static_cast<int>(candidates.size()) > m_maxTiles)
        candidates.resize(m_maxTiles);

    std::set<TileKey> wantedSet;
    for (const auto& c : candidates)
        wantedSet.insert({c.tx, c.ty});

    // --- Step 1: Evict rendering cache only (DON'T touch dtNavMesh) ---
    auto it = m_cache.begin();
    while (it != m_cache.end()) {
        if (wantedSet.count(it->first) == 0)
            it = m_cache.erase(it);
        else
            ++it;
    }

    // --- Step 2: Populate rendering cache for wanted tiles ---
    int diskLoads = 0;
    for (const auto& c : candidates) {
        TileKey key = {c.tx, c.ty};
        if (m_cache.count(key))
            continue;

        // Ensure tile is in dtNavMesh (may load from disk, throttled)
        if (!EnsureTileInNavmesh(c.tx, c.ty, centerTX, centerTY, diskLoads))
            continue; // disk budget exhausted or tile file missing

        // Get the actual Detour coords from the navmesh tile entry
        auto mit = m_meshTiles.find(key);
        if (mit == m_meshTiles.end())
            continue;

        int dtx = mit->second.detourX;
        int dty = mit->second.detourY;

        // Extract triangles using exact Detour coords (no FP conversion)
        CacheEntry entry;
        entry.detourX = dtx;
        entry.detourY = dty;
        entry.triangles = ExtractTriangles(m_mesh, c.tx, c.ty, dtx, dty);

        // Snapshot dtMeshHeader into cache so metadata survives navmesh eviction
        {
            const dtMeshTile* tile = m_mesh->getTileAt(dtx, dty, 0);
            if (tile && tile->header) {
                const dtMeshHeader* hdr = tile->header;
                auto& hi = entry.headerInfo;
                hi.valid = true;
                hi.tileX = c.tx;
                hi.tileY = c.ty;
                hi.detourX = dtx;
                hi.detourY = dty;
                hi.polyCount = hdr->polyCount;
                hi.vertCount = hdr->vertCount;
                hi.detailTriCount = hdr->detailTriCount;
                hi.detailVertCount = hdr->detailVertCount;
                hi.detailMeshCount = hdr->detailMeshCount;
                hi.bvNodeCount = hdr->bvNodeCount;
                hi.offMeshConCount = hdr->offMeshConCount;
                hi.maxLinkCount = hdr->maxLinkCount;
                hi.walkableHeight = hdr->walkableHeight;
                hi.walkableRadius = hdr->walkableRadius;
                hi.walkableClimb = hdr->walkableClimb;
            }
        }

        if (!entry.triangles.triangles.empty())
            m_cache[key] = std::move(entry);
    }
}

void TileCache::UpdateViewport3D(float targetX, float targetY, float cameraDistance) {
    if (!m_mesh) return;

    // Center tile from camera target
    int centerTX = 31 - static_cast<int>(std::floor(targetX / kTileSize));
    int centerTY = 31 - static_cast<int>(std::floor(targetY / kTileSize));

    // Loading radius: proportional to camera distance, in tile units
    // At distance 200 (default) load ~5 tiles radius; at 2000 load ~15
    float radiusYards = std::max(cameraDistance * 2.0f, 500.0f);
    int radiusTiles = static_cast<int>(std::ceil(radiusYards / kTileSize));
    radiusTiles = std::clamp(radiusTiles, 2, 20);

    int tMinX = std::clamp(centerTX - radiusTiles, 0, 63);
    int tMaxX = std::clamp(centerTX + radiusTiles, 0, 63);
    int tMinY = std::clamp(centerTY - radiusTiles, 0, 63);
    int tMaxY = std::clamp(centerTY + radiusTiles, 0, 63);

    // Build list of tiles sorted by distance from center (nearest first)
    struct TileDist {
        int tx, ty;
        int dist;
    };
    std::vector<TileDist> candidates;
    candidates.reserve((tMaxX - tMinX + 1) * (tMaxY - tMinY + 1));

    for (int tx = tMinX; tx <= tMaxX; ++tx) {
        for (int ty = tMinY; ty <= tMaxY; ++ty) {
            int dx = tx - centerTX;
            int dy = ty - centerTY;
            int d2 = dx * dx + dy * dy;
            if (d2 <= radiusTiles * radiusTiles)
                candidates.push_back({tx, ty, d2});
        }
    }

    std::sort(candidates.begin(), candidates.end(),
              [](const TileDist& a, const TileDist& b) { return a.dist < b.dist; });

    if (static_cast<int>(candidates.size()) > m_maxTiles)
        candidates.resize(m_maxTiles);

    std::set<TileKey> wantedSet;
    for (const auto& c : candidates)
        wantedSet.insert({c.tx, c.ty});

    // Evict rendering cache entries not in wanted set
    auto it = m_cache.begin();
    while (it != m_cache.end()) {
        if (wantedSet.count(it->first) == 0)
            it = m_cache.erase(it);
        else
            ++it;
    }

    // Populate rendering cache for wanted tiles (same logic as 2D)
    int diskLoads = 0;
    for (const auto& c : candidates) {
        TileKey key = {c.tx, c.ty};
        if (m_cache.count(key))
            continue;

        if (!EnsureTileInNavmesh(c.tx, c.ty, centerTX, centerTY, diskLoads))
            continue;

        auto mit = m_meshTiles.find(key);
        if (mit == m_meshTiles.end())
            continue;

        int dtx = mit->second.detourX;
        int dty = mit->second.detourY;

        CacheEntry entry;
        entry.detourX = dtx;
        entry.detourY = dty;
        entry.triangles = ExtractTriangles(m_mesh, c.tx, c.ty, dtx, dty);

        {
            const dtMeshTile* tile = m_mesh->getTileAt(dtx, dty, 0);
            if (tile && tile->header) {
                const dtMeshHeader* hdr = tile->header;
                auto& hi = entry.headerInfo;
                hi.valid = true;
                hi.tileX = c.tx;
                hi.tileY = c.ty;
                hi.detourX = dtx;
                hi.detourY = dty;
                hi.polyCount = hdr->polyCount;
                hi.vertCount = hdr->vertCount;
                hi.detailTriCount = hdr->detailTriCount;
                hi.detailVertCount = hdr->detailVertCount;
                hi.detailMeshCount = hdr->detailMeshCount;
                hi.bvNodeCount = hdr->bvNodeCount;
                hi.offMeshConCount = hdr->offMeshConCount;
                hi.maxLinkCount = hdr->maxLinkCount;
                hi.walkableHeight = hdr->walkableHeight;
                hi.walkableRadius = hdr->walkableRadius;
                hi.walkableClimb = hdr->walkableClimb;
            }
        }

        if (!entry.triangles.triangles.empty())
            m_cache[key] = std::move(entry);
    }
}

bool TileCache::EnsureTileInNavmesh(int tileX, int tileY, int centerTX, int centerTY,
                                     int& diskLoads) {
    TileKey key = {tileX, tileY};

    // Already in navmesh — just update access order
    auto mit = m_meshTiles.find(key);
    if (mit != m_meshTiles.end()) {
        mit->second.accessOrder = ++m_accessCounter;
        return true;
    }

    // Known missing tile — skip immediately (no disk I/O)
    if (m_missingTiles.count(key))
        return false;

    // Need to load from disk — check throttle
    if (diskLoads >= kMaxLoadsPerFrame)
        return false;

    // Evict LRU navmesh tile if at capacity
    if (static_cast<int>(m_meshTiles.size()) >= kMaxNavmeshTiles)
        EvictFarthestNavmeshTile(centerTX, centerTY);

    // Load from disk into dtNavMesh, capturing actual Detour coords
    int dtx = 0, dty = 0;
    if (!LoadTileIntoMesh(m_mesh, m_mmapDir, m_mapId, tileX, tileY,
                          nullptr, &dtx, &dty)) {
        m_missingTiles.insert(key);
        return false;
    }

    ++diskLoads;

    NavmeshTileEntry entry;
    entry.accessOrder = ++m_accessCounter;
    entry.detourX = dtx;
    entry.detourY = dty;
    m_meshTiles[key] = entry;
    return true;
}

void TileCache::EvictFarthestNavmeshTile(int centerTX, int centerTY) {
    if (m_meshTiles.empty()) return;

    // Find the tile with the lowest accessOrder (least recently used)
    auto victim = m_meshTiles.begin();
    for (auto it = m_meshTiles.begin(); it != m_meshTiles.end(); ++it) {
        if (it->second.accessOrder < victim->second.accessOrder)
            victim = it;
    }

    // Remove from dtNavMesh using exact Detour coords (always correct)
    UnloadTileFromMeshAt(m_mesh, victim->second.detourX, victim->second.detourY);

    // NOTE: do NOT erase from m_cache — rendering data (TileTriangles) is self-contained
    // and survives navmesh tile eviction. With 64 navmesh slots and 200 visible tiles,
    // the navmesh acts as a pipeline stage for triangle extraction.

    m_meshTiles.erase(victim);
}

const TileTriangles* TileCache::GetTriangles(int tileX, int tileY) const {
    auto it = m_cache.find({tileX, tileY});
    if (it == m_cache.end()) return nullptr;
    return &it->second.triangles;
}

void TileCache::ForEachTile(const TileCallback& cb) const {
    for (const auto& [key, entry] : m_cache)
        cb(key.first, key.second, entry.triangles, entry.detourX, entry.detourY);
}

TileInfo TileCache::GetTileInfo(int tileX, int tileY) const {
    TileKey key = {tileX, tileY};

    // Start from cached header snapshot (survives navmesh eviction)
    auto cit = m_cache.find(key);
    if (cit != m_cache.end()) {
        TileInfo info = cit->second.headerInfo;  // copy snapshot
        info.tileX = tileX;
        info.tileY = tileY;
        info.valid = true;
        info.inRenderCache = true;
        info.renderTriCount = static_cast<int>(cit->second.triangles.triangles.size());
        info.inNavmesh = (m_meshTiles.count(key) > 0);
        return info;
    }

    // Not in render cache — check navmesh directly
    auto mit = m_meshTiles.find(key);
    if (mit != m_meshTiles.end()) {
        TileInfo info;
        info.tileX = tileX;
        info.tileY = tileY;
        info.valid = true;
        info.inNavmesh = true;
        info.detourX = mit->second.detourX;
        info.detourY = mit->second.detourY;

        if (m_mesh) {
            const dtMeshTile* tile = m_mesh->getTileAt(
                mit->second.detourX, mit->second.detourY, 0);
            if (tile && tile->header) {
                const dtMeshHeader* hdr = tile->header;
                info.polyCount = hdr->polyCount;
                info.vertCount = hdr->vertCount;
                info.detailTriCount = hdr->detailTriCount;
                info.detailVertCount = hdr->detailVertCount;
                info.detailMeshCount = hdr->detailMeshCount;
                info.bvNodeCount = hdr->bvNodeCount;
                info.offMeshConCount = hdr->offMeshConCount;
                info.maxLinkCount = hdr->maxLinkCount;
                info.walkableHeight = hdr->walkableHeight;
                info.walkableRadius = hdr->walkableRadius;
                info.walkableClimb = hdr->walkableClimb;
            }
        }
        return info;
    }

    TileInfo info;
    info.tileX = tileX;
    info.tileY = tileY;
    return info;
}

void TileCache::Clear() {
    if (m_query) {
        dtFreeNavMeshQuery(m_query);
        m_query = nullptr;
    }
    if (m_mesh) {
        dtFreeNavMesh(m_mesh);
        m_mesh = nullptr;
    }
    m_cache.clear();
    m_meshTiles.clear();
    m_missingTiles.clear();
    m_accessCounter = 0;
    m_mapId = 0xFFFFFFFF;
}

} // namespace mapedit
