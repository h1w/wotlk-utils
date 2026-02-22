#pragma once
#include <cstdint>

namespace mapedit {

struct Canvas;
struct Camera3D;

class StatusBar {
public:
    void Render(const Canvas& canvas, uint32_t mapId, const char* mapName);
    void Render3D(const Camera3D& camera, uint32_t mapId, const char* mapName);
};

} // namespace mapedit
