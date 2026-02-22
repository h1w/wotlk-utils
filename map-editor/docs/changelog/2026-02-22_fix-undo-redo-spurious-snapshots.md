# Fix undo/redo creating spurious entries on every mouse click

**Date:** 2026-02-22

## Summary

Fixed the undo/redo system creating history entries for non-mutating actions (selecting nodes, clicking empty space, right-clicking). Moved from speculative pre-click snapshots to precise per-mutation snapshots. Also added proper undo support for property panel edits with descriptive labels.

## Problem

Every left-click, right-click, and double-click in the canvas viewport created an undo entry — even when the click didn't change any data. Clicking a node to select it, clicking empty space to deselect, or right-clicking with no effect all polluted the undo stack with no-op entries. This made Ctrl+Z unpredictable: users had to press it multiple times to undo a single real change.

**Root cause:** `app.cpp` took a speculative snapshot on every discrete mouse/keyboard event before editors processed the input, regardless of whether the event would lead to an actual mutation.

```cpp
// OLD (app.cpp:494-510) — snapshot on EVERY click
bool discreteAction = (inCanvas &&
    (ImGui::IsMouseClicked(Left) || IsMouseClicked(Right) ||
     IsMouseDoubleClicked(Left))) ||
    (!WantTextInput && IsKeyPressed(Delete));
if (discreteAction)
    m_undoRedo.Snapshot(m_graphData, m_routeData, "Edit");
```

## Solution

Removed the speculative snapshot block entirely. Instead, each editor now calls `Snapshot()` right before the exact line that mutates data. A new `UndoContext` helper struct wraps `UndoRedo&`, `WorldGraphData&`, and `RouteData&` and is passed to all editors.

Two snapshot modes:
- **`Snapshot(desc)`** — always creates a new entry (for discrete one-shot mutations: add, delete, move-start)
- **`SnapshotIfNeeded(desc)`** — only creates an entry if the last undo description differs (coalesces repeated same-type edits like typing characters into one undo step)

For drag operations (Move Node, Move Waypoint), a 2-pixel movement threshold prevents click-without-drag from creating an entry.

## Changes

### `editor/undo_redo.h`
- Added `UndoContext` struct with `Snapshot()` and `SnapshotIfNeeded()` methods

### `editor/graph_editor.h` / `graph_editor.cpp`
- Added `UndoContext&` parameter to `ProcessInput()`
- Added `m_dragSnapshotTaken` flag for drag-start detection
- Snapshot before: Delete Node, Delete Edge, Create Edge, Add Node (double-click), Move Node (drag start with threshold)

### `editor/route_editor.h` / `route_editor.cpp`
- Added `UndoContext&` parameter to `ProcessInput()` and `RenderPanel()`
- Added `m_dragWpSnapshotTaken` flag for waypoint drag-start detection
- Snapshot before: Add/Delete/Move Waypoint, New Route, Delete Route, route property edits (name, color, loop)

### `ui/property_panel.h` / `property_panel.cpp`
- Added `UndoContext&` parameter to `Render()`
- Snapshot before: all node property edits (name, X/Y/Z, map ID, type, faction), all edge property edits (type, cost, bidirectional), Delete Node/Edge buttons
- For widgets that modify values directly (InputFloat, Checkbox): save-restore pattern ensures the snapshot captures pre-mutation state

### `app.cpp`
- Removed the `discreteAction` speculative snapshot block (lines 494-510)
- Creates `UndoContext` and passes it to all editors and panels

## What now creates undo steps (only actual mutations)

| Action | Description | Method |
|---|---|---|
| Double-click canvas | Add Node | `Snapshot` |
| Delete key / button | Delete Node / Delete Edge | `Snapshot` |
| Drag node (2px+ movement) | Move Node | `Snapshot` (once at drag start) |
| Edge mode: click second node | Create Edge | `Snapshot` |
| Click empty space in route edit | Add Waypoint | `Snapshot` |
| Right-click waypoint | Delete Waypoint | `Snapshot` |
| Drag waypoint (2px+ movement) | Move Waypoint | `Snapshot` (once at drag start) |
| "New Route" / "Delete Route" button | New Route / Delete Route | `Snapshot` |
| Edit node/edge/route properties | Edit Node Name, Edit Node X, etc. | `SnapshotIfNeeded` |

## What no longer creates undo steps

- Left-click to select a node or edge
- Left-click on empty space to deselect
- Right-click in canvas with no action
- Click-and-release on a node without dragging (below 2px threshold)

## Bonus: descriptive undo labels

Undo descriptions changed from a generic "Edit" for everything to specific labels like "Move Node", "Delete Edge", "Edit Node Name", "Add Waypoint", etc. These appear in the Edit menu ("Undo Move Node", "Redo Delete Edge").
