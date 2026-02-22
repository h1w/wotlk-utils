#include "tile_loader.h"

#include <DetourNavMesh.h>
#include <DetourNavMeshQuery.h>
#include <DetourStatus.h>
#include <DetourCommon.h>

#include <glog/logging.h>

#include <cstdio>
#include <cstring>
#include <cmath>

namespace mapedit {

// TrinityCore MMAP file structures
#pragma pack(push, 1)
struct MmapNavMeshHeader {
    uint32_t mmapMagic;
    uint32_t mmapVersion;
    dtNavMeshParams params;
    uint32_t offmeshConnectionCount;
};

struct MmapTileHeader {
    uint32_t mmapMagic;
    uint32_t dtVersion;
    uint32_t mmapVersion;
    uint32_t size;
    char     usesLiquids;
    char     padding[3];
};
#pragma pack(pop)

static constexpr uint32_t MMAP_MAGIC = 0x4D4D4150;
static constexpr int kMaxTiles = 64;
static constexpr int kMaxNodes = 65535;

dtNavMesh* CreateNavMesh(const std::string& mmapDir, uint32_t mapId) {
    char filename[512];
    snprintf(filename, sizeof(filename), "%s%03u.mmap", mmapDir.c_str(), mapId);

    FILE* f = fopen(filename, "rb");
    if (!f) {
        LOG(ERROR) << "[TileLoader] Cannot open: " << filename;
        return nullptr;
    }

    fseek(f, 0, SEEK_END);
    long fileSize = ftell(f);
    fseek(f, 0, SEEK_SET);

    dtNavMeshParams params{};

    if (fileSize >= static_cast<long>(sizeof(MmapNavMeshHeader))) {
        MmapNavMeshHeader header;
        if (fread(&header, 1, sizeof(header), f) != sizeof(header)) {
            fclose(f);
            return nullptr;
        }
        if (header.mmapMagic != MMAP_MAGIC) {
            fclose(f);
            return nullptr;
        }
        params = header.params;
    } else if (fileSize == sizeof(dtNavMeshParams)) {
        if (fread(&params, 1, sizeof(params), f) != sizeof(params)) {
            fclose(f);
            return nullptr;
        }
    } else {
        fclose(f);
        return nullptr;
    }
    fclose(f);

    // Override for 32-bit dtPolyRef compatibility
    params.maxTiles = kMaxTiles;

    unsigned int nextPow2 = 1;
    while (nextPow2 < static_cast<unsigned int>(params.maxTiles))
        nextPow2 <<= 1;

    unsigned int tileBits = 0;
    {
        unsigned int v = nextPow2;
        while (v > 1) { v >>= 1; tileBits++; }
    }

    // Detour requires saltBits >= 10 (DetourNavMesh.cpp:260).
    // tileBits(6) + saltBits(10) + polyBits(16) = 32  →  maxPolys = 65536
    static constexpr unsigned int kSaltBits = 10;
    unsigned int polyBits = 32 - tileBits - kSaltBits;
    params.maxPolys = 1u << polyBits;

    dtNavMesh* mesh = dtAllocNavMesh();
    if (!mesh) return nullptr;

    dtStatus status = mesh->init(&params);
    if (dtStatusFailed(status)) {
        dtFreeNavMesh(mesh);
        return nullptr;
    }

    LOG(INFO) << "[TileLoader] Created navmesh for map " << mapId
              << " (maxTiles=" << params.maxTiles
              << ", maxPolys=" << params.maxPolys << ")";
    return mesh;
}

dtNavMeshQuery* CreateNavMeshQuery(dtNavMesh* mesh) {
    if (!mesh) return nullptr;

    dtNavMeshQuery* query = dtAllocNavMeshQuery();
    if (!query) return nullptr;

    if (dtStatusFailed(query->init(mesh, kMaxNodes))) {
        dtFreeNavMeshQuery(query);
        return nullptr;
    }
    return query;
}

bool LoadTileIntoMesh(dtNavMesh* mesh, const std::string& mmapDir,
                      uint32_t mapId, int tileX, int tileY,
                      unsigned int* outTileRef,
                      int* outDetourX, int* outDetourY) {
    if (!mesh) return false;

    char filename[512];
    snprintf(filename, sizeof(filename), "%s%03u%02d%02d.mmtile",
             mmapDir.c_str(), mapId, tileX, tileY);

    FILE* f = fopen(filename, "rb");
    if (!f) return false;

    MmapTileHeader header;
    if (fread(&header, 1, sizeof(header), f) != sizeof(header)) {
        fclose(f);
        return false;
    }

    if (header.mmapMagic != MMAP_MAGIC || header.size == 0) {
        fclose(f);
        return false;
    }

    unsigned char* data = static_cast<unsigned char*>(
        dtAlloc(header.size, DT_ALLOC_PERM));
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

    // Repack TC 16-byte dtLink -> stock 12-byte dtLink
    int tileDataSize = static_cast<int>(header.size);
    {
        static constexpr int kTcDtLinkSize = 16;

        const dtMeshHeader* hdr = reinterpret_cast<const dtMeshHeader*>(data);

        const int headerSz       = dtAlign4(sizeof(dtMeshHeader));
        const int vertsSz        = dtAlign4(sizeof(float) * 3 * hdr->vertCount);
        const int polysSz        = dtAlign4(sizeof(dtPoly) * hdr->polyCount);
        const int tcLinksSz      = dtAlign4(kTcDtLinkSize * hdr->maxLinkCount);
        const int stockLinksSz   = dtAlign4(static_cast<int>(sizeof(dtLink)) * hdr->maxLinkCount);
        const int detailMeshesSz = dtAlign4(sizeof(dtPolyDetail) * hdr->detailMeshCount);
        const int detailVertsSz  = dtAlign4(sizeof(float) * 3 * hdr->detailVertCount);
        const int detailTrisSz   = dtAlign4(sizeof(unsigned char) * 4 * hdr->detailTriCount);
        const int bvTreeSz       = dtAlign4(sizeof(dtBVNode) * hdr->bvNodeCount);
        const int offMeshConsSz  = dtAlign4(sizeof(dtOffMeshConnection) * hdr->offMeshConCount);

        const int preLinksSz  = headerSz + vertsSz + polysSz;
        const int postLinksSz = detailMeshesSz + detailVertsSz + detailTrisSz
                              + bvTreeSz + offMeshConsSz;
        const int tcTotal    = preLinksSz + tcLinksSz + postLinksSz;
        const int stockTotal = preLinksSz + stockLinksSz + postLinksSz;

        if (tcLinksSz != stockLinksSz && tcTotal > tileDataSize) {
            LOG(WARNING) << "[TileLoader] Repack skipped for tile (" << tileX << ","
                         << tileY << "): tcTotal=" << tcTotal
                         << " > fileSize=" << tileDataSize;
        }

        if (tcLinksSz != stockLinksSz && tcTotal <= tileDataSize) {
            unsigned char* repacked = static_cast<unsigned char*>(
                dtAlloc(stockTotal, DT_ALLOC_PERM));
            if (repacked) {
                memset(repacked, 0, stockTotal);
                // Copy header + verts + polys
                memcpy(repacked, data, preLinksSz);
                // Copy everything after links (detail meshes, BV tree, etc.)
                memcpy(repacked + preLinksSz + stockLinksSz,
                       data + preLinksSz + tcLinksSz,
                       postLinksSz);
                dtFree(data);
                data = repacked;
                tileDataSize = stockTotal;
            }
        }
    }

    // Guard: remove any existing tile at the same Detour position to prevent ghosts
    {
        const dtMeshHeader* hdr = reinterpret_cast<const dtMeshHeader*>(data);

        // Output actual Detour coords from tile header (avoids FP conversion errors)
        if (outDetourX) *outDetourX = hdr->x;
        if (outDetourY) *outDetourY = hdr->y;

        dtTileRef existing = mesh->getTileRefAt(hdr->x, hdr->y, hdr->layer);
        if (existing) {
            LOG(WARNING) << "[TileLoader] Ghost tile at (" << hdr->x << "," << hdr->y
                         << ") — removing before re-add";
            mesh->removeTile(existing, nullptr, nullptr);
        }
    }

    dtTileRef tileRef = 0;
    dtStatus status = mesh->addTile(data, tileDataSize, DT_TILE_FREE_DATA, 0, &tileRef);
    if (dtStatusFailed(status)) {
        LOG(WARNING) << "[TileLoader] addTile failed for (" << tileX << "," << tileY
                     << ") status=0x" << std::hex << status << std::dec
                     << " dataSize=" << tileDataSize;
        dtFree(data);
        return false;
    }

    if (outTileRef)
        *outTileRef = tileRef;
    return true;
}

// Convert WoW tile grid (tileX, tileY) to Detour tile grid (dtx, dty)
// using the navmesh origin and tile sizes.
static void WowTileToDetourTile(const dtNavMesh* mesh, int tileX, int tileY,
                                 int& dtx, int& dty) {
    static constexpr float kTS = 533.33333f;
    // Center of WoW tile in world coords: tile covers [(31-t)*TS, (32-t)*TS)
    float wowX = (31 - tileX + 0.5f) * kTS;
    float wowY = (31 - tileY + 0.5f) * kTS;
    // Convert to Detour: [0]=wowY, [1]=wowZ, [2]=wowX
    float pos[3] = { wowY, 0.0f, wowX };
    mesh->calcTileLoc(pos, &dtx, &dty);
}

void UnloadTileFromMeshAt(dtNavMesh* mesh, int detourX, int detourY) {
    if (!mesh) return;
    dtTileRef ref = mesh->getTileRefAt(detourX, detourY, 0);
    if (ref)
        mesh->removeTile(ref, nullptr, nullptr);
}

void UnloadTileFromMesh(dtNavMesh* mesh, int tileX, int tileY) {
    if (!mesh) return;
    int dtx, dty;
    WowTileToDetourTile(mesh, tileX, tileY, dtx, dty);
    dtTileRef ref = mesh->getTileRefAt(dtx, dty, 0);
    if (ref)
        mesh->removeTile(ref, nullptr, nullptr);
}

// Helper: extract detail mesh triangles from a tile at known Detour coords.
static TileTriangles ExtractTrianglesImpl(const dtNavMesh* mesh, int tileX, int tileY,
                                            int dtx, int dty) {
    TileTriangles result;
    result.tileX = tileX;
    result.tileY = tileY;

    const dtMeshTile* tile = mesh->getTileAt(dtx, dty, 0);
    if (!tile || !tile->header) return result;

    const dtMeshHeader* hdr = tile->header;

    for (int p = 0; p < hdr->polyCount; ++p) {
        const dtPoly& poly = tile->polys[p];
        if (poly.getType() == DT_POLYTYPE_OFFMESH_CONNECTION)
            continue;

        const dtPolyDetail& pd = tile->detailMeshes[p];

        for (unsigned int t2 = 0; t2 < pd.triCount; ++t2) {
            const unsigned char* tri = &tile->detailTris[(pd.triBase + t2) * 4];
            TileTriangle tt;

            for (int v = 0; v < 3; ++v) {
                const float* vp;
                if (tri[v] < poly.vertCount)
                    vp = &tile->verts[poly.verts[tri[v]] * 3];
                else
                    vp = &tile->detailVerts[(pd.vertBase + tri[v] - poly.vertCount) * 3];

                // Detour -> WoW: wowX = d[2], wowY = d[0], wowZ = d[1]
                tt.x[v] = vp[2];
                tt.y[v] = vp[0];
                tt.z[v] = vp[1];
            }

            result.triangles.push_back(tt);
        }
    }

    return result;
}

// Helper: extract base polygons from a tile at known Detour coords.
static TileTriangles ExtractBasePolygonsImpl(const dtNavMesh* mesh, int tileX, int tileY,
                                               int dtx, int dty) {
    TileTriangles result;
    result.tileX = tileX;
    result.tileY = tileY;

    const dtMeshTile* tile = mesh->getTileAt(dtx, dty, 0);
    if (!tile || !tile->header) return result;

    const dtMeshHeader* hdr = tile->header;

    for (int p = 0; p < hdr->polyCount; ++p) {
        const dtPoly& poly = tile->polys[p];
        if (poly.getType() == DT_POLYTYPE_OFFMESH_CONNECTION)
            continue;

        for (int i = 1; i + 1 < poly.vertCount; ++i) {
            const float* v0 = &tile->verts[poly.verts[0] * 3];
            const float* v1 = &tile->verts[poly.verts[i] * 3];
            const float* v2 = &tile->verts[poly.verts[i + 1] * 3];

            TileTriangle tt;
            tt.x[0] = v0[2]; tt.y[0] = v0[0]; tt.z[0] = v0[1];
            tt.x[1] = v1[2]; tt.y[1] = v1[0]; tt.z[1] = v1[1];
            tt.x[2] = v2[2]; tt.y[2] = v2[0]; tt.z[2] = v2[1];

            result.triangles.push_back(tt);
        }
    }

    return result;
}

TileTriangles ExtractTriangles(const dtNavMesh* mesh, int tileX, int tileY,
                                int detourX, int detourY) {
    if (!mesh) return TileTriangles{tileX, tileY, {}};
    return ExtractTrianglesImpl(mesh, tileX, tileY, detourX, detourY);
}

TileTriangles ExtractTriangles(const dtNavMesh* mesh, int tileX, int tileY) {
    if (!mesh) return TileTriangles{tileX, tileY, {}};
    int dtx, dty;
    WowTileToDetourTile(mesh, tileX, tileY, dtx, dty);
    return ExtractTrianglesImpl(mesh, tileX, tileY, dtx, dty);
}

TileTriangles ExtractBasePolygons(const dtNavMesh* mesh, int tileX, int tileY,
                                   int detourX, int detourY) {
    if (!mesh) return TileTriangles{tileX, tileY, {}};
    return ExtractBasePolygonsImpl(mesh, tileX, tileY, detourX, detourY);
}

TileTriangles ExtractBasePolygons(const dtNavMesh* mesh, int tileX, int tileY) {
    if (!mesh) return TileTriangles{tileX, tileY, {}};
    int dtx, dty;
    WowTileToDetourTile(mesh, tileX, tileY, dtx, dty);
    return ExtractBasePolygonsImpl(mesh, tileX, tileY, dtx, dty);
}

} // namespace mapedit
