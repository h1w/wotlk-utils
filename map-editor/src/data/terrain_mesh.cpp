#include "terrain_mesh.h"
#include "terrain_loader.h"

#include <cmath>
#include <algorithm>

namespace mapedit {

static constexpr float TILE_SIZE = 533.33333f;
static constexpr float CELL_SIZE = TILE_SIZE / 128.0f;

static void ComputeVertexNormals(TerrainMesh& mesh) {
    // Zero out normals
    for (auto& v : mesh.vertices) {
        v.nx = v.ny = v.nz = 0.0f;
    }

    // Accumulate face normals at each vertex
    for (size_t i = 0; i + 2 < mesh.indices.size(); i += 3) {
        uint32_t i0 = mesh.indices[i];
        uint32_t i1 = mesh.indices[i + 1];
        uint32_t i2 = mesh.indices[i + 2];

        const auto& v0 = mesh.vertices[i0];
        const auto& v1 = mesh.vertices[i1];
        const auto& v2 = mesh.vertices[i2];

        float e1x = v1.x - v0.x, e1y = v1.y - v0.y, e1z = v1.z - v0.z;
        float e2x = v2.x - v0.x, e2y = v2.y - v0.y, e2z = v2.z - v0.z;

        float nx = e1y * e2z - e1z * e2y;
        float ny = e1z * e2x - e1x * e2z;
        float nz = e1x * e2y - e1y * e2x;

        mesh.vertices[i0].nx += nx;  mesh.vertices[i0].ny += ny;  mesh.vertices[i0].nz += nz;
        mesh.vertices[i1].nx += nx;  mesh.vertices[i1].ny += ny;  mesh.vertices[i1].nz += nz;
        mesh.vertices[i2].nx += nx;  mesh.vertices[i2].ny += ny;  mesh.vertices[i2].nz += nz;
    }

    // Normalize
    for (auto& v : mesh.vertices) {
        float len = std::sqrt(v.nx * v.nx + v.ny * v.ny + v.nz * v.nz);
        if (len > 1e-8f) {
            float inv = 1.0f / len;
            v.nx *= inv;  v.ny *= inv;  v.nz *= inv;
        } else {
            v.nx = 0.0f;  v.ny = 0.0f;  v.nz = 1.0f;
        }
    }
}

void GenerateTerrainMesh(const TerrainTileData& tile, int tileX, int tileY,
                          TerrainMesh& out, int decimation) {
    out.tileX = tileX;
    out.tileY = tileY;
    out.vertices.clear();
    out.indices.clear();

    // Clamp decimation to valid power-of-2 that divides 128
    if (decimation < 1) decimation = 1;
    if (decimation > 8) decimation = 8;
    // Round down to nearest power of 2 that divides 128
    if (decimation >= 8)      decimation = 8;
    else if (decimation >= 4) decimation = 4;
    else if (decimation >= 2) decimation = 2;
    else                      decimation = 1;

    float tileOriginX = (32 - tileX) * TILE_SIZE;   // north edge
    float tileOriginY = (32 - tileY) * TILE_SIZE;   // west edge

    int cellsPerSide = 128 / decimation;
    int v9Rows = cellsPerSide + 1;
    int v9Cols = cellsPerSide + 1;
    int v9Offset = 0;
    int v8Offset = v9Rows * v9Cols;

    out.vertices.reserve(v9Rows * v9Cols + cellsPerSide * cellsPerSide);

    // V9 corner vertices (subsampled by decimation)
    for (int r = 0; r < v9Rows; ++r) {
        for (int c = 0; c < v9Cols; ++c) {
            int srcRow = r * decimation;
            int srcCol = c * decimation;
            TerrainVertex v;
            v.x = tileOriginX - srcRow * CELL_SIZE;
            v.y = tileOriginY - srcCol * CELL_SIZE;
            v.z = tile.v9[srcRow * 129 + srcCol];
            v.nx = v.ny = 0.0f; v.nz = 1.0f;
            out.vertices.push_back(v);
        }
    }

    // V8 center vertices (averaged over the DxD block)
    for (int r = 0; r < cellsPerSide; ++r) {
        for (int c = 0; c < cellsPerSide; ++c) {
            float avgZ = 0.0f;
            int count = 0;
            for (int dr = 0; dr < decimation; ++dr) {
                for (int dc = 0; dc < decimation; ++dc) {
                    avgZ += tile.v8[(r * decimation + dr) * 128 + (c * decimation + dc)];
                    ++count;
                }
            }
            avgZ /= static_cast<float>(count);

            float centerRow = r * decimation + decimation * 0.5f;
            float centerCol = c * decimation + decimation * 0.5f;

            TerrainVertex v;
            v.x = tileOriginX - centerRow * CELL_SIZE;
            v.y = tileOriginY - centerCol * CELL_SIZE;
            v.z = avgZ;
            v.nx = v.ny = 0.0f; v.nz = 1.0f;
            out.vertices.push_back(v);
        }
    }

    // Index buffer: 4 triangles per decimated cell (fan from V8 center)
    out.indices.reserve(cellsPerSide * cellsPerSide * 4 * 3);

    for (int r = 0; r < cellsPerSide; ++r) {
        for (int c = 0; c < cellsPerSide; ++c) {
            // Hole check: skip cell if ANY sub-cell in the DxD block has a hole
            if (tile.hasHoles) {
                bool holed = false;
                for (int dr = 0; dr < decimation && !holed; ++dr) {
                    for (int dc = 0; dc < decimation && !holed; ++dc) {
                        int srcR = r * decimation + dr;
                        int srcC = c * decimation + dc;
                        int holeRow = srcR / 8;
                        int holeCol = srcC / 8;
                        int subRow = (srcR % 8) / 2;
                        int subCol = (srcC % 8) / 2;
                        uint16_t holeMask = tile.holes[holeRow * 16 + holeCol];
                        if (holeMask & (1 << (subRow * 4 + subCol)))
                            holed = true;
                    }
                }
                if (holed) continue;
            }

            uint32_t tl = v9Offset + r * v9Cols + c;
            uint32_t tr = v9Offset + r * v9Cols + c + 1;
            uint32_t bl = v9Offset + (r + 1) * v9Cols + c;
            uint32_t br = v9Offset + (r + 1) * v9Cols + c + 1;
            uint32_t ct = v8Offset + r * cellsPerSide + c;

            // 4 triangles (fan from center)
            out.indices.push_back(tl); out.indices.push_back(tr); out.indices.push_back(ct);
            out.indices.push_back(tr); out.indices.push_back(br); out.indices.push_back(ct);
            out.indices.push_back(br); out.indices.push_back(bl); out.indices.push_back(ct);
            out.indices.push_back(bl); out.indices.push_back(tl); out.indices.push_back(ct);
        }
    }

    ComputeVertexNormals(out);

    // Compute AABB
    out.minX = out.minY = out.minZ =  1e30f;
    out.maxX = out.maxY = out.maxZ = -1e30f;
    for (const auto& v : out.vertices) {
        out.minX = (std::min)(out.minX, v.x);
        out.minY = (std::min)(out.minY, v.y);
        out.minZ = (std::min)(out.minZ, v.z);
        out.maxX = (std::max)(out.maxX, v.x);
        out.maxY = (std::max)(out.maxY, v.y);
        out.maxZ = (std::max)(out.maxZ, v.z);
    }
}

} // namespace mapedit
