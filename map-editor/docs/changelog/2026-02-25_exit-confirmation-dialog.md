# Exit Confirmation Dialog

**Date:** 2026-02-25

## Summary

Added an "Unsaved Changes" confirmation popup when closing the application (window close button, Alt+F4, or File > Exit) while any data has unsaved modifications. Prevents accidental loss of graph/route edits.

## Problem

The application had dirty-flag tracking (`IsDirty()`) on `WorldGraphData` and `RouteData`, and a yellow "Unsaved changes" indicator in the property panel, but closing the window silently discarded all unsaved edits without any warning.

## Solution

Intercept all exit paths and show an ImGui modal popup with three options when unsaved changes exist. If no changes are dirty, exit immediately without prompting.

## Changes

### WndProc — WM_CLOSE handler (`app.cpp`)

- Added `WM_CLOSE` case that calls `RequestQuit()` and returns 0 (prevents default `DestroyWindow`)
- Previously only `WM_DESTROY` was handled; `WM_CLOSE` fell through to `DefWindowProc` which destroyed the window unconditionally

### RequestQuit (`app.cpp`)

- New method: checks `HasUnsavedChanges()` — if true, sets `m_showExitConfirm` flag to trigger the popup; if false, calls `DestroyWindow` directly

### HasUnsavedChanges (`app.cpp`)

- New `const` method: returns true if any of `m_graphData`, `m_roadGraphData`, or `m_routeData` is both loaded and dirty

### SaveAllDirty (`app.cpp`)

- New method: saves all loaded+dirty data sources to their current file paths and clears dirty flags
- Consolidates the save logic previously duplicated in the Ctrl+S handler

### ImGui Modal Popup (`app.cpp`, in `RenderFrame`)

- Rendered in `RenderFrame()` (not inside `RenderFrame2D`/`RenderFrame3D`) so it works in both view modes
- Popup name: "Unsaved Changes"
- Flags: `AlwaysAutoResize | NoMove`
- Three buttons:
  - **Save & Exit** — calls `SaveAllDirty()` then `PostQuitMessage(0)`
  - **Don't Save** — calls `PostQuitMessage(0)` directly (discards changes)
  - **Cancel** — closes popup, resumes editing

### Exit Menu Item (`app.cpp`)

- Changed `File > Exit` from `PostQuitMessage(0)` to `RequestQuit()` so it goes through the same unsaved-changes check

### App Header (`app.h`)

- Added `m_showExitConfirm` flag
- Declared `HasUnsavedChanges()`, `SaveAllDirty()`, `RequestQuit()`

## Modified Files

| File | Change |
|------|--------|
| `src/app.h` | Added flag + 3 method declarations |
| `src/app.cpp` | WM_CLOSE handler, popup rendering, 3 new methods, Exit menu fix |
