#pragma once
#include <cstdint>
#include "selection.h"

namespace mapedit {

struct Canvas;
struct UndoContext;
class WorldGraphData;

class GraphEditor {
public:
    // Process mouse input for node/edge interaction
    // Returns true if the editor consumed the input
    bool ProcessInput(const Canvas& canvas, WorldGraphData& graph,
                      Selection& selection, uint32_t mapId,
                      UndoContext& undo);

    bool IsEdgeMode() const { return m_edgeMode; }
    void SetEdgeMode(bool on) { m_edgeMode = on; }

    bool IsDragging() const { return m_dragging; }

private:
    bool m_edgeMode = false;
    bool m_dragging = false;
    bool m_dragSnapshotTaken = false;
    uint32_t m_edgeStartNode = 0;

    // Hit test: find node near screen position
    uint32_t HitTestNode(const Canvas& canvas, const WorldGraphData& graph,
                         uint32_t mapId, float sx, float sy, float radius = 10.0f);

    // Hit test: find edge near screen position
    int HitTestEdge(const Canvas& canvas, const WorldGraphData& graph,
                    uint32_t mapId, float sx, float sy, float threshold = 8.0f);
};

} // namespace mapedit
