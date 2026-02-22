#pragma once
#include <cstdint>

namespace mapedit {

struct Camera3D;
class Primitives3D;
class RouteData;
class RouteEditor;

class Route3DRenderer {
public:
    void Render(const Camera3D& camera, Primitives3D& prims,
                const RouteData& routes, const RouteEditor& editor,
                uint32_t mapId);
};

} // namespace mapedit
