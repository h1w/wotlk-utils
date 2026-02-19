#include "nav_mesh.h"

#include <DetourNavMesh.h>
#include <DetourNavMeshQuery.h>
#include <DetourStatus.h>
#include <DetourCommon.h>

#include <glog/logging.h>

#include <cstdio>
#include <cstring>
#include <cmath>

namespace nav {

// ---------------------------------------------------------------------------
// TrinityCore MMAP file structures
// ---------------------------------------------------------------------------

#pragma pack(push, 1)
struct MmapNavMeshHeader {
    uint32_t mmapMagic;        // 0x4D4D4150 ("MMAP")
    uint32_t mmapVersion;      // 16
    dtNavMeshParams params;
    uint32_t offmeshConnectionCount;
};

struct MmapTileHeader {
    uint32_t mmapMagic;        // 0x4D4D4150
    uint32_t dtVersion;
    uint32_t mmapVersion;
    uint32_t size;             // Detour tile data blob size in bytes
    char     usesLiquids;
    char     padding[3];
};
#pragma pack(pop)

static constexpr uint32_t MMAP_MAGIC   = 0x4D4D4150; // "MMAP"
static constexpr uint32_t MMAP_VERSION = 16;

// WoW tile size in yards (one ADT = 533.333... yards)
static constexpr float TILE_SIZE = 533.33333f;

// ---------------------------------------------------------------------------
// Singleton
// ---------------------------------------------------------------------------

NavMesh& NavMesh::Instance() {
    static NavMesh s_instance;
    return s_instance;
}

NavMesh::~NavMesh() {
    Shutdown();
}

// ---------------------------------------------------------------------------
// Initialize / Shutdown
// ---------------------------------------------------------------------------

bool NavMesh::Initialize(const char* mmapDir) {
    if (!mmapDir || mmapDir[0] == '\0') {
        LOG(ERROR) << "[Nav] Initialize: mmapDir is empty";
        return false;
    }

    m_mmapDir = mmapDir;
    // Ensure trailing slash
    if (m_mmapDir.back() != '/' && m_mmapDir.back() != '\\')
        m_mmapDir += '/';

    LOG(INFO) << "[Nav] Initialized with mmaps dir: " << m_mmapDir;
    return true;
}

void NavMesh::Shutdown() {
    FreeNavMesh();
    m_mmapDir.clear();
    LOG(INFO) << "[Nav] Shutdown";
}

void NavMesh::FreeNavMesh() {
    m_loadedTiles.clear();
    m_failedMaps.clear();

    if (m_query) {
        dtFreeNavMeshQuery(m_query);
        m_query = nullptr;
    }
    if (m_navMesh) {
        dtFreeNavMesh(m_navMesh);
        m_navMesh = nullptr;
    }

    m_mapId = 0xFFFFFFFF;
    m_lastCenterTileX = -999;
    m_lastCenterTileY = -999;
}

// ---------------------------------------------------------------------------
// LoadMap — read .mmap header and create dtNavMesh
// ---------------------------------------------------------------------------

bool NavMesh::LoadMap(uint32_t mapId) {
    if (m_mapId == mapId && m_navMesh)
        return true; // already loaded

    if (m_failedMaps.count(mapId))
        return false; // already tried and failed — don't spam

    // Free previous map
    FreeNavMesh();

    // Build header filename: {mapId:03d}.mmap (TrinityCore format)
    char filename[512];
    snprintf(filename, sizeof(filename), "%s%03u.mmap", m_mmapDir.c_str(), mapId);

    FILE* f = fopen(filename, "rb");
    if (!f) {
        LOG(ERROR) << "[Nav] Cannot open mmap header: " << filename;
        m_failedMaps.insert(mapId);
        return false;
    }

    // Determine file size
    fseek(f, 0, SEEK_END);
    long fileSize = ftell(f);
    fseek(f, 0, SEEK_SET);

    dtNavMeshParams params{};

    if (fileSize >= static_cast<long>(sizeof(MmapNavMeshHeader))) {
        // New format: MmapNavMeshHeader (magic + version + params + offmeshCount)
        MmapNavMeshHeader header;
        size_t rd = fread(&header, 1, sizeof(header), f);
        fclose(f);

        if (rd != sizeof(header)) {
            LOG(ERROR) << "[Nav] Invalid mmap header in " << filename;
            m_failedMaps.insert(mapId);
            return false;
        }

        if (header.mmapMagic != MMAP_MAGIC) {
            LOG(ERROR) << "[Nav] Bad mmap magic: 0x" << std::hex << header.mmapMagic;
            m_failedMaps.insert(mapId);
            return false;
        }

        params = header.params;
        LOG(INFO) << "[Nav] Loaded mmap header (new format) for map " << mapId;

    } else if (fileSize == sizeof(dtNavMeshParams)) {
        // Old format: raw dtNavMeshParams only (28 bytes, no magic/version)
        size_t rd = fread(&params, 1, sizeof(params), f);
        fclose(f);

        if (rd != sizeof(params)) {
            LOG(ERROR) << "[Nav] Failed to read dtNavMeshParams from " << filename;
            m_failedMaps.insert(mapId);
            return false;
        }

        LOG(INFO) << "[Nav] Loaded mmap header (legacy format, " << fileSize << " bytes) for map " << mapId;

    } else {
        fclose(f);
        LOG(ERROR) << "[Nav] Unknown mmap header size (" << fileSize << " bytes) in " << filename;
        m_failedMaps.insert(mapId);
        return false;
    }

    // --- Fix for 32-bit dtPolyRef compatibility ---
    // TrinityCore generates mmaps with maxPolys=0x80000000 (needs 31 poly bits),
    // which requires 64-bit dtPolyRef (DT_POLYREF64). Stock Detour uses 32-bit
    // refs, so we must override maxPolys/maxTiles to fit the 32-bit bit budget:
    //   polyBits + tileBits + saltBits(>=10) <= 32
    // We only load ~9 tiles at a time (3x3 grid), so a small maxTiles suffices.
    {
        unsigned int origMaxPolys = params.maxPolys;
        unsigned int origMaxTiles = params.maxTiles;

        // Cap maxTiles: we only need ~9 active tiles, 64 slots is generous
        if (params.maxTiles > kMaxTilesOverride)
            params.maxTiles = kMaxTilesOverride;

        // Cap maxPolys to fit 32-bit poly ref encoding
        // tileBits = ilog2(nextPow2(maxTiles)), we want polyBits + tileBits + 10 <= 32
        unsigned int nextPow2Tiles = 1;
        while (nextPow2Tiles < (unsigned int)params.maxTiles) nextPow2Tiles <<= 1;
        unsigned int tileBits = 0;
        { unsigned int v = nextPow2Tiles; while (v > 1) { v >>= 1; tileBits++; } }
        unsigned int maxPolyBits = 32 - tileBits - 10; // reserve 10 salt bits
        unsigned int maxPolysAllowed = 1u << maxPolyBits;
        // Always override — the original value (0x80000000) is for 64-bit dtPolyRef
        params.maxPolys = maxPolysAllowed;

        LOG(INFO) << "[Nav] Overriding params for 32-bit dtPolyRef: maxTiles "
                  << origMaxTiles << " -> " << params.maxTiles
                  << ", maxPolys 0x" << std::hex << origMaxPolys
                  << " -> 0x" << params.maxPolys << std::dec
                  << " (tileBits=" << tileBits << ", polyBits=" << maxPolyBits
                  << ", saltBits=10)";
    }

    // Create dtNavMesh
    m_navMesh = dtAllocNavMesh();
    if (!m_navMesh) {
        LOG(ERROR) << "[Nav] dtAllocNavMesh failed";
        m_failedMaps.insert(mapId);
        return false;
    }

    dtStatus status = m_navMesh->init(&params);
    if (dtStatusFailed(status)) {
        LOG(ERROR) << "[Nav] dtNavMesh::init failed, status=0x" << std::hex << status;
        dtFreeNavMesh(m_navMesh);
        m_navMesh = nullptr;
        m_failedMaps.insert(mapId);
        return false;
    }

    // Create query object
    m_query = dtAllocNavMeshQuery();
    if (!m_query) {
        LOG(ERROR) << "[Nav] dtAllocNavMeshQuery failed";
        dtFreeNavMesh(m_navMesh);
        m_navMesh = nullptr;
        m_failedMaps.insert(mapId);
        return false;
    }

    static constexpr int kMaxNodes = 2048;
    status = m_query->init(m_navMesh, kMaxNodes);
    if (dtStatusFailed(status)) {
        LOG(ERROR) << "[Nav] dtNavMeshQuery::init failed, status=0x" << std::hex << status;
        dtFreeNavMeshQuery(m_query);
        m_query = nullptr;
        dtFreeNavMesh(m_navMesh);
        m_navMesh = nullptr;
        m_failedMaps.insert(mapId);
        return false;
    }

    m_mapId = mapId;
    LOG(INFO) << "[Nav] Loaded map " << mapId
              << " (maxTiles=" << params.maxTiles
              << ", maxPolys=" << params.maxPolys << ")";
    return true;
}

// ---------------------------------------------------------------------------
// Tile loading
// ---------------------------------------------------------------------------

bool NavMesh::LoadTile(int tileX, int tileY) {
    auto key = std::make_pair(tileX, tileY);
    if (m_loadedTiles.count(key))
        return true; // already loaded

    if (!m_navMesh) return false;

    // Build tile filename: {mapId:03d}{gridX:02d}{gridY:02d}.mmtile
    // gridX = tileX (from WoW X), gridY = tileY (from WoW Y)
    char filename[512];
    snprintf(filename, sizeof(filename), "%s%03u%02d%02d.mmtile",
             m_mmapDir.c_str(), m_mapId, tileX, tileY);

    FILE* f = fopen(filename, "rb");
    if (!f)
        return false; // tile doesn't exist — normal for ocean/empty areas

    MmapTileHeader header;
    if (fread(&header, 1, sizeof(header), f) != sizeof(header)) {
        fclose(f);
        return false;
    }

    if (header.mmapMagic != MMAP_MAGIC || header.size == 0) {
        fclose(f);
        return false;
    }

    // Allocate buffer for Detour (Detour takes ownership via dtFree)
    unsigned char* data = static_cast<unsigned char*>(dtAlloc(header.size, DT_ALLOC_PERM));
    if (!data) {
        fclose(f);
        return false;
    }

    if (fread(data, 1, header.size, f) != header.size) {
        dtFree(data);
        fclose(f);
        return false;
    }
    fclose(f);

    // --- Repack tile data: TC uses 16-byte dtLink (64-bit dtPolyRef), stock uses 12-byte ---
    // TC's Detour has DT_POLYREF64 enabled, making dtLink 16 bytes (8-byte dtPolyRef ref).
    // Stock Detour uses 32-bit dtPolyRef, so dtLink is 12 bytes. The link section in TC's
    // binary tile data is maxLinkCount*16 bytes, but stock Detour reads it as maxLinkCount*12,
    // which shifts all subsequent sections (detailMeshes, BV tree, etc.) by maxLinkCount*4
    // bytes, corrupting the tile.
    int tileDataSize = static_cast<int>(header.size);
    {
        static constexpr int kTcDtLinkSize = 16; // sizeof(dtLink) with DT_POLYREF64

        const dtMeshHeader* hdr = reinterpret_cast<const dtMeshHeader*>(data);

        // Log struct sizes once to verify assumptions
        static bool s_loggedSizes = false;
        if (!s_loggedSizes) {
            s_loggedSizes = true;
            LOG(INFO) << "[Nav] Struct sizes: dtMeshHeader=" << sizeof(dtMeshHeader)
                      << " dtPoly=" << sizeof(dtPoly) << " dtLink=" << sizeof(dtLink)
                      << " dtPolyDetail=" << sizeof(dtPolyDetail) << " dtBVNode=" << sizeof(dtBVNode)
                      << " dtOffMeshConnection=" << sizeof(dtOffMeshConnection);
        }

        // Save header values before any free
        const int hdrVertCount      = hdr->vertCount;
        const int hdrPolyCount      = hdr->polyCount;
        const int hdrMaxLinkCount   = hdr->maxLinkCount;
        const int hdrDetailMeshCnt  = hdr->detailMeshCount;
        const int hdrDetailVertCnt  = hdr->detailVertCount;
        const int hdrDetailTriCnt   = hdr->detailTriCount;
        const int hdrBvNodeCount    = hdr->bvNodeCount;
        const int hdrOffMeshConCnt  = hdr->offMeshConCount;

        const int headerSz       = dtAlign4(sizeof(dtMeshHeader));
        const int vertsSz        = dtAlign4(sizeof(float) * 3 * hdrVertCount);
        const int polysSz        = dtAlign4(sizeof(dtPoly) * hdrPolyCount);
        const int tcLinksSz      = dtAlign4(kTcDtLinkSize * hdrMaxLinkCount);
        const int stockLinksSz   = dtAlign4(static_cast<int>(sizeof(dtLink)) * hdrMaxLinkCount);
        const int detailMeshesSz = dtAlign4(sizeof(dtPolyDetail) * hdrDetailMeshCnt);
        const int detailVertsSz  = dtAlign4(sizeof(float) * 3 * hdrDetailVertCnt);
        const int detailTrisSz   = dtAlign4(sizeof(unsigned char) * 4 * hdrDetailTriCnt);
        const int bvTreeSz       = dtAlign4(sizeof(dtBVNode) * hdrBvNodeCount);
        const int offMeshConsSz  = dtAlign4(sizeof(dtOffMeshConnection) * hdrOffMeshConCnt);

        const int preLinksSz  = headerSz + vertsSz + polysSz;
        const int postLinksSz = detailMeshesSz + detailVertsSz + detailTrisSz
                              + bvTreeSz + offMeshConsSz;

        const int tcTotal    = preLinksSz + tcLinksSz + postLinksSz;
        const int stockTotal = preLinksSz + stockLinksSz + postLinksSz;

        // Log detailed sizes only for first tile
        static bool s_loggedFirstTile = false;
        if (!s_loggedFirstTile) {
            s_loggedFirstTile = true;
            LOG(INFO) << "[Nav] Tile (" << tileX << "," << tileY << ") header: verts="
                      << hdrVertCount << " polys=" << hdrPolyCount << " maxLinks=" << hdrMaxLinkCount
                      << " detailMesh=" << hdrDetailMeshCnt << " detailVert=" << hdrDetailVertCnt
                      << " detailTri=" << hdrDetailTriCnt << " bvNodes=" << hdrBvNodeCount
                      << " offMesh=" << hdrOffMeshConCnt;
            LOG(INFO) << "[Nav] Tile sizes: pre=" << preLinksSz
                      << " tcLinks=" << tcLinksSz << " stockLinks=" << stockLinksSz
                      << " post=" << postLinksSz << " tcTotal=" << tcTotal
                      << " stockTotal=" << stockTotal << " fileSize=" << tileDataSize;
        }

        if (tcLinksSz != stockLinksSz && tcTotal <= tileDataSize) {
            unsigned char* repacked = static_cast<unsigned char*>(
                dtAlloc(stockTotal, DT_ALLOC_PERM));
            if (repacked) {
                memset(repacked, 0, stockTotal);

                // Copy header + verts + polys (identical offsets)
                memcpy(repacked, data, preLinksSz);

                // Link section is zeroed — addTile rebuilds links as a freelist

                // Copy post-links sections (detailMeshes through offMeshCons)
                memcpy(repacked + preLinksSz + stockLinksSz,
                       data + preLinksSz + tcLinksSz,
                       postLinksSz);

                dtFree(data);
                data = repacked;
                tileDataSize = stockTotal;

                static bool s_loggedRepack = false;
                if (!s_loggedRepack) {
                    s_loggedRepack = true;
                    LOG(INFO) << "[Nav] Repacked tile (" << tileX << "," << tileY
                              << ") for 32-bit dtPolyRef: " << header.size << " -> " << stockTotal
                              << " bytes (saved " << (tcLinksSz - stockLinksSz) << ")";
                }
            } else {
                LOG(ERROR) << "[Nav] Failed to allocate repacked buffer (" << stockTotal << " bytes)";
            }
        } else if (tcLinksSz == stockLinksSz) {
            LOG(INFO) << "[Nav] Tile (" << tileX << "," << tileY << ") links already stock size, no repack needed";
        } else {
            LOG(WARNING) << "[Nav] Tile (" << tileX << "," << tileY << ") repack SKIPPED: tcTotal="
                         << tcTotal << " > fileSize=" << tileDataSize;
        }
    }

    dtTileRef tileRef = 0;
    dtStatus status = m_navMesh->addTile(data, tileDataSize, DT_TILE_FREE_DATA, 0, &tileRef);
    if (dtStatusFailed(status)) {
        LOG(ERROR) << "[Nav] addTile failed for (" << tileX << "," << tileY
                   << ") status=0x" << std::hex << status;
        dtFree(data);
        return false;
    }

    m_loadedTiles[key] = tileRef;
    LOG(INFO) << "[Nav] Loaded tile (" << tileX << "," << tileY
              << ") ref=" << tileRef << " size=" << tileDataSize;
    return true;
}

void NavMesh::UnloadTile(int tileX, int tileY) {
    auto key = std::make_pair(tileX, tileY);
    auto it = m_loadedTiles.find(key);
    if (it == m_loadedTiles.end())
        return;

    if (m_navMesh)
        m_navMesh->removeTile(it->second, nullptr, nullptr);

    m_loadedTiles.erase(it);
}

// ---------------------------------------------------------------------------
// Tile streaming
// ---------------------------------------------------------------------------

void NavMesh::WorldToTile(float x, float y, int& tileX, int& tileY) {
    // WoW coordinate system: X grows south, Y grows west
    // TrinityCore tile mapping: tile = 32 - floor(coord / TILE_SIZE)
    // Must use floor (not truncation) for correct results with negative coordinates
    tileX = 32 - static_cast<int>(std::floor(x / TILE_SIZE));
    tileY = 32 - static_cast<int>(std::floor(y / TILE_SIZE));
}

void NavMesh::UpdateLoadedTiles(float x, float y) {
    if (!m_navMesh) return;

    int centerTileX, centerTileY;
    WorldToTile(x, y, centerTileX, centerTileY);

    // Skip if we haven't moved to a new tile
    if (centerTileX == m_lastCenterTileX && centerTileY == m_lastCenterTileY)
        return;

    m_lastCenterTileX = centerTileX;
    m_lastCenterTileY = centerTileY;

    // Determine desired tile set
    std::map<std::pair<int,int>, bool> desired;
    for (int dx = -kTileLoadRadius; dx <= kTileLoadRadius; ++dx) {
        for (int dy = -kTileLoadRadius; dy <= kTileLoadRadius; ++dy) {
            desired[{centerTileX + dx, centerTileY + dy}] = true;
        }
    }

    // Unload tiles no longer needed
    std::vector<std::pair<int,int>> toUnload;
    for (auto& [key, ref] : m_loadedTiles) {
        if (!desired.count(key))
            toUnload.push_back(key);
    }
    for (auto& key : toUnload)
        UnloadTile(key.first, key.second);

    // Load new tiles
    for (auto& [key, _] : desired) {
        if (!m_loadedTiles.count(key))
            LoadTile(key.first, key.second);
    }
}

} // namespace nav
