#pragma once
#include <cstdint>

namespace mapedit {

class WorldGraphData;
struct Selection;
struct Canvas;
struct UndoContext;
class TileCache;

class PropertyPanel {
public:
    void Render(WorldGraphData& graph, Selection& selection, uint32_t mapId,
                const Canvas& canvas, TileCache& tileCache, UndoContext& undo);
};

} // namespace mapedit
