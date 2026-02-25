#pragma once
#include <cstdint>

namespace mapedit {

struct Camera3D;
class Primitives3D;
class WorldGraphData;
struct MultiSelection;
struct LayerVisibility;

class Graph3DRenderer {
public:
    // Render graph. dimAlpha < 1.0 renders as a dim overlay (inactive graph).
    void Render(const Camera3D& camera, Primitives3D& prims,
                const WorldGraphData& graph, uint32_t mapId,
                const MultiSelection& selection, const LayerVisibility& layers,
                float dimAlpha = 1.0f);
};

} // namespace mapedit
