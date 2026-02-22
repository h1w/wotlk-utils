#pragma once
#include <cstdint>

namespace mapedit {

struct Canvas;
class TileIndex;

class GridRenderer {
public:
    void Render(const Canvas& canvas, const TileIndex& tileIndex, uint32_t mapId);

private:
    void RenderCoordGrid(const Canvas& canvas);
    void RenderTileGrid(const Canvas& canvas, const TileIndex& tileIndex, uint32_t mapId);
};

} // namespace mapedit
