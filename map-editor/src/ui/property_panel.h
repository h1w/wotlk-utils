#pragma once
#include <cstdint>

namespace mapedit {

class WorldGraphData;
struct MultiSelection;
struct Canvas;
struct UndoContext;
class TileCache;

class PropertyPanel {
public:
    void Render(WorldGraphData& graph, MultiSelection& selection, uint32_t mapId,
                const Canvas& canvas, TileCache& tileCache, UndoContext& undo);
};

} // namespace mapedit
