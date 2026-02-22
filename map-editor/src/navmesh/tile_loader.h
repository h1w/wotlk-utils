#pragma once

#include <cstdint>
#include <string>
#include <vector>

class dtNavMesh;
class dtNavMeshQuery;

namespace mapedit {

struct TileTriangle {
    float x[3]; // WoW X coords of 3 vertices
    float y[3]; // WoW Y coords
    float z[3]; // WoW Z coords
};

struct TileTriangles {
    int tileX = 0;
    int tileY = 0;
    std::vector<TileTriangle> triangles;
};

// Create a dtNavMesh configured for the map editor (generous limits).
// Reads the .mmap header file and creates the mesh with overridden maxTiles/maxPolys.
dtNavMesh* CreateNavMesh(const std::string& mmapDir, uint32_t mapId);

// Create a query object for the mesh
dtNavMeshQuery* CreateNavMeshQuery(dtNavMesh* mesh);

// Load a single tile into the navmesh. Handles TC->stock repack.
// Returns true on success. If outTileRef is non-null, receives the dtTileRef
// for later removal via dtNavMesh::removeTile.
// outDetourX/Y receive the actual Detour tile coords from the tile header
// (avoids floating-point WoW→Detour coordinate conversion errors).
bool LoadTileIntoMesh(dtNavMesh* mesh, const std::string& mmapDir,
                      uint32_t mapId, int tileX, int tileY,
                      unsigned int* outTileRef = nullptr,
                      int* outDetourX = nullptr, int* outDetourY = nullptr);

// Remove a tile from the navmesh using Detour coords directly.
void UnloadTileFromMeshAt(dtNavMesh* mesh, int detourX, int detourY);

// Remove a tile from the navmesh (WoW coords → Detour conversion, may be imprecise).
void UnloadTileFromMesh(dtNavMesh* mesh, int tileX, int tileY);

// Extract triangles using known Detour tile coords (preferred — no FP conversion).
TileTriangles ExtractTriangles(const dtNavMesh* mesh, int tileX, int tileY,
                                int detourX, int detourY);

// Extract triangles (WoW coords → Detour conversion, may miss boundary tiles).
TileTriangles ExtractTriangles(const dtNavMesh* mesh, int tileX, int tileY);

// Extract base polygons using known Detour tile coords (preferred).
TileTriangles ExtractBasePolygons(const dtNavMesh* mesh, int tileX, int tileY,
                                   int detourX, int detourY);

// Extract base polygons (WoW coords → Detour conversion, may miss boundary tiles).
TileTriangles ExtractBasePolygons(const dtNavMesh* mesh, int tileX, int tileY);

} // namespace mapedit
