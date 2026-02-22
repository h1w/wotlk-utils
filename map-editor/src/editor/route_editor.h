#pragma once
#include <cstdint>

namespace mapedit {

struct Canvas;
struct UndoContext;
class RouteData;

class RouteEditor {
public:
    void RenderPanel(RouteData& data, uint32_t mapId, UndoContext& undo);
    bool ProcessInput(const Canvas& canvas, RouteData& data, uint32_t mapId,
                      UndoContext& undo);

    int GetSelectedRoute() const { return m_selectedRoute; }
    int GetSelectedWaypoint() const { return m_selectedWaypoint; }
    bool IsEditing() const { return m_editing; }

private:
    int m_selectedRoute = -1;
    int m_selectedWaypoint = -1;
    bool m_editing = false;
    bool m_draggingWaypoint = false;
    bool m_dragWpSnapshotTaken = false;
};

} // namespace mapedit
