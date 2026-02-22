#pragma once

namespace mapedit {

class CompassWidget {
public:
    // rotation: 0 for 2D (fixed south-up), camera yaw for 3D
    void Render(float vpX, float vpY, float vpW, float vpH, float rotation);
};

} // namespace mapedit
