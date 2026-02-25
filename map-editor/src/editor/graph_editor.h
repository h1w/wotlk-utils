#pragma once
#include <cstdint>
#include <unordered_map>
#include <utility>
#include <vector>
#include <imgui.h>
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
                      MultiSelection& selection, uint32_t mapId,
                      UndoContext& undo);

    bool IsEdgeMode() const { return m_edgeMode; }
    void SetEdgeMode(bool on) { m_edgeMode = on; }

    bool IsDrawMode() const { return m_drawMode; }
    void SetDrawMode(bool on) { m_drawMode = on; m_drawLastNode = 0; }

    bool IsDragging() const { return m_drag.active; }
    bool IsBoxSelecting() const { return m_boxSelecting; }

private:
    bool m_edgeMode = false;
    uint32_t m_edgeStartNode = 0;

    // Draw mode: click to place nodes, auto-connect to previous
    bool m_drawMode = false;
    uint32_t m_drawLastNode = 0;

    // Drag state (multi-node)
    struct DragState {
        bool active = false;
        bool snapshotTaken = false;
        float anchorWx = 0, anchorWy = 0;
        std::unordered_map<uint32_t, std::pair<float,float>> startPositions;
    };
    DragState m_drag;

    // Box selection
    bool  m_boxSelecting = false;
    float m_boxStartX = 0, m_boxStartY = 0;

    // Context menu
    bool     m_wantOpenContextMenu = false;
    float    m_contextMenuWx = 0, m_contextMenuWy = 0;
    uint32_t m_contextHitNode = 0;
    int      m_contextHitEdge = -1;

    // Lasso selection
    bool m_lassoSelecting = false;
    std::vector<ImVec2> m_lassoPoints;

    // Validation results
    bool m_showValidation = false;
    int  m_valComponents = 0;
    int  m_valDeadEnds = 0;
    int  m_valDuplicates = 0;
    int  m_valZeroLength = 0;
    int  m_valOrphans = 0;
    std::vector<uint32_t> m_valDeadEndNodes;
    std::vector<uint32_t> m_valOrphanNodes;
    std::vector<size_t>   m_valDuplicateEdges;

    // Render context menu + validation popups (called every frame before WantCaptureMouse check)
    bool RenderPopups(const Canvas& canvas, WorldGraphData& graph,
                      MultiSelection& selection, uint32_t mapId,
                      UndoContext& undo);

    // Hit test: find node near screen position
    uint32_t HitTestNode(const Canvas& canvas, const WorldGraphData& graph,
                         uint32_t mapId, float sx, float sy, float radius = 10.0f);

    // Hit test: find edge near screen position
    int HitTestEdge(const Canvas& canvas, const WorldGraphData& graph,
                    uint32_t mapId, float sx, float sy, float threshold = 8.0f);
};

} // namespace mapedit
