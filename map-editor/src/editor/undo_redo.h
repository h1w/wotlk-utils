#pragma once

#include "../data/world_graph_data.h"
#include "../data/route_data.h"

#include <deque>
#include <string>
#include <vector>

namespace mapedit {

// Stores a complete snapshot of graph + route state.
struct EditorSnapshot {
    std::vector<WorldNode> nodes;
    std::vector<WorldEdge> edges;
    std::vector<Route>     routes;
    std::string            description;
};

// Snapshot-based undo/redo system.
// Call Snapshot() BEFORE each mutation to capture the current state.
// Then call Undo()/Redo() to navigate history.
class UndoRedo {
public:
    static constexpr size_t kMaxDepth = 100;

    // Capture current state before a mutation.
    // |description| is a short label like "Move Node", "Delete Edge", etc.
    void Snapshot(const WorldGraphData& graph,
                  const RouteData& routeData,
                  const std::string& description);

    // Undo the last action.  Returns true if an undo was performed.
    // Pushes current state onto the redo stack, then restores the previous state.
    bool Undo(WorldGraphData& graph, RouteData& routeData);

    // Redo the last undone action.  Returns true if a redo was performed.
    // Pushes current state onto the undo stack, then restores the redo state.
    bool Redo(WorldGraphData& graph, RouteData& routeData);

    bool   CanUndo()    const { return !m_undoStack.empty(); }
    bool   CanRedo()    const { return !m_redoStack.empty(); }
    size_t UndoCount()  const { return m_undoStack.size(); }
    size_t RedoCount()  const { return m_redoStack.size(); }

    const char* UndoDescription() const {
        return m_undoStack.empty() ? "" : m_undoStack.back().description.c_str();
    }
    const char* RedoDescription() const {
        return m_redoStack.empty() ? "" : m_redoStack.back().description.c_str();
    }

    // Clear all history (e.g., when loading a new file).
    void Clear();

private:
    std::deque<EditorSnapshot> m_undoStack;
    std::deque<EditorSnapshot> m_redoStack;

    EditorSnapshot CaptureState(const WorldGraphData& graph,
                                const RouteData& routeData,
                                const std::string& desc) const;
    void RestoreState(const EditorSnapshot& snap,
                      WorldGraphData& graph,
                      RouteData& routeData);
};

// Helper for passing undo context to editors.
// Wraps references to the undo system and both data stores so editors
// can take snapshots right before mutations without knowing about each other.
struct UndoContext {
    UndoRedo& undoRedo;
    WorldGraphData& graph;
    RouteData& routeData;

    // Always take a new snapshot (for one-shot mutations: add/delete/move-start).
    void Snapshot(const std::string& desc) {
        undoRedo.Snapshot(graph, routeData, desc);
    }

    // Take snapshot only if the last undo entry has a different description.
    // Coalesces repeated same-type edits (e.g. typing characters) into one step.
    void SnapshotIfNeeded(const std::string& desc) {
        if (!undoRedo.CanUndo() || desc != undoRedo.UndoDescription())
            undoRedo.Snapshot(graph, routeData, desc);
    }
};

} // namespace mapedit
