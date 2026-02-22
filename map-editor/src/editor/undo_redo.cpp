#include "undo_redo.h"
#include "../data/world_graph_data.h"
#include "../data/route_data.h"

namespace mapedit {

void UndoRedo::Snapshot(const WorldGraphData& graph,
                        const RouteData& routeData,
                        const std::string& description) {
    // Any new action invalidates the redo history.
    m_redoStack.clear();

    m_undoStack.push_back(CaptureState(graph, routeData, description));

    // Enforce max depth.
    while (m_undoStack.size() > kMaxDepth)
        m_undoStack.pop_front();
}

bool UndoRedo::Undo(WorldGraphData& graph, RouteData& routeData) {
    if (m_undoStack.empty())
        return false;

    // Save current state to redo stack before restoring.
    m_redoStack.push_back(CaptureState(graph, routeData,
                                       m_undoStack.back().description));

    // Pop the most recent snapshot and restore it.
    EditorSnapshot snap = std::move(m_undoStack.back());
    m_undoStack.pop_back();
    RestoreState(snap, graph, routeData);

    return true;
}

bool UndoRedo::Redo(WorldGraphData& graph, RouteData& routeData) {
    if (m_redoStack.empty())
        return false;

    // Save current state to undo stack before restoring.
    m_undoStack.push_back(CaptureState(graph, routeData,
                                       m_redoStack.back().description));

    // Pop the most recent redo snapshot and restore it.
    EditorSnapshot snap = std::move(m_redoStack.back());
    m_redoStack.pop_back();
    RestoreState(snap, graph, routeData);

    return true;
}

void UndoRedo::Clear() {
    m_undoStack.clear();
    m_redoStack.clear();
}

EditorSnapshot UndoRedo::CaptureState(const WorldGraphData& graph,
                                      const RouteData& routeData,
                                      const std::string& desc) const {
    EditorSnapshot snap;
    snap.nodes       = graph.GetNodes();   // deep copy
    snap.edges       = graph.GetEdges();   // deep copy
    snap.routes      = routeData.GetRoutes(); // deep copy
    snap.description = desc;
    return snap;
}

void UndoRedo::RestoreState(const EditorSnapshot& snap,
                            WorldGraphData& graph,
                            RouteData& routeData) {
    graph.SetState(snap.nodes, snap.edges);
    routeData.SetState(snap.routes);
}

} // namespace mapedit
