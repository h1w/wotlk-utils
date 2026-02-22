#pragma once

namespace mapedit {

struct Camera3D;
class Primitives3D;
class PathRenderer;

// Renders the PathRenderer's current A/B points and computed path in 3D,
// reading state directly from the existing 2D PathRenderer instance.
class Path3DRenderer {
public:
    void Render(const Camera3D& camera, Primitives3D& prims,
                const PathRenderer& pathRenderer);
};

} // namespace mapedit
