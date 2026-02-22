#pragma once
#include <cstdint>

namespace mapedit {

struct Camera3D;
class Primitives3D;
class WorldGraphData;
struct Selection;
struct LayerVisibility;

class Graph3DRenderer {
public:
    void Render(const Camera3D& camera, Primitives3D& prims,
                const WorldGraphData& graph, uint32_t mapId,
                const Selection& selection, const LayerVisibility& layers);
};

} // namespace mapedit
