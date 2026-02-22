#pragma once
#include <cstdint>

namespace mapedit {

struct Canvas;
class RouteData;
class RouteEditor;

class RouteRenderer {
public:
    void Render(const Canvas& canvas, const RouteData& data,
                const RouteEditor& editor, uint32_t mapId);
};

} // namespace mapedit
