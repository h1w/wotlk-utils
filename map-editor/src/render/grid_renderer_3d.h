#pragma once
#include <cstdint>

namespace mapedit {

struct Camera3D;
class Primitives3D;
class TileIndex;

// Draws tile boundary rectangles as 3D lines at Z=0.
class Grid3DRenderer {
public:
    void Render(const Camera3D& camera, Primitives3D& prims,
                const TileIndex& tileIndex, uint32_t mapId);
};

} // namespace mapedit
