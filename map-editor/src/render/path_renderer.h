#pragma once
#include <vector>

namespace mapedit {

struct Canvas;
class TileCache;
class MapPathfinder;

enum class PathTestState {
    Idle,
    SetA,
    SetB,
};

class PathRenderer {
public:
    // 2D point stored in the cached path (XY only — Z is discarded by the 2D renderer)
    struct PathPt { float x, y; };

    void Render(const Canvas& canvas);
    bool ProcessInput(const Canvas& canvas, TileCache& cache, MapPathfinder& pathfinder);

    PathTestState GetState() const { return m_state; }
    void SetState(PathTestState s) { m_state = s; }
    void Reset();
    bool HasResult() const { return m_hasA || m_hasB || m_path.valid; }

    // Accessors used by Path3DRenderer
    bool  HasA() const { return m_hasA; }
    bool  HasB() const { return m_hasB; }
    float AX() const { return m_ax; }
    float AY() const { return m_ay; }
    float AZ() const { return m_az; }
    float BX() const { return m_bx; }
    float BY() const { return m_by; }
    float BZ() const { return m_bz; }

    bool  PathValid()    const { return m_path.valid; }
    bool  PathPartial()  const { return m_path.partial; }
    float PathDistance() const { return m_path.distance; }
    const std::vector<PathPt>& PathPts() const { return m_path.pts; }

private:
    PathTestState m_state = PathTestState::Idle;

    float m_ax = 0, m_ay = 0, m_az = 0;
    float m_bx = 0, m_by = 0, m_bz = 0;
    bool m_hasA = false;
    bool m_hasB = false;

    struct CachedPath {
        bool valid = false;
        bool partial = false;
        float distance = 0;
        std::vector<PathPt> pts;
    } m_path;
};

} // namespace mapedit
