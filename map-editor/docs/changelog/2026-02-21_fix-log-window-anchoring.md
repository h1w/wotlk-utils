# Fix log window anchoring to bottom-left corner

**Date:** 2026-02-21

## Summary

Fixed the log window floating freely around the application after first render. It is now permanently anchored to the bottom-left corner, matching the legend widget's behavior.

## Problem

The log window used `ImGuiCond_FirstUseEver` for its position, meaning ImGui only set the position on the first frame. After that, the window could be dragged anywhere in the application, losing its intended dock position.

## Fix

Changed `log_window.cpp` Render() to pin the window to the bottom-left corner:

- **`ImGuiCond_Always`** — position recalculated every frame, stays anchored on window resize
- **Pivot `(0.0, 1.0)`** — anchors from the left-bottom corner of the window
- **`ImGuiWindowFlags_NoMove`** — prevents dragging
- **`ImGuiWindowFlags_NoFocusOnAppearing | NoNav`** — does not steal focus, consistent with legend
- **`SetNextWindowBgAlpha(0.75f)`** — semi-transparent background, consistent with legend
- 10px margin from the viewport edge

Window size remains user-resizable (`ImGuiCond_FirstUseEver` kept for size).

## Files changed

| File | Change |
|------|--------|
| `src/ui/log_window.cpp` | Pin position to bottom-left with `ImGuiCond_Always`, add `NoMove`/`NoFocusOnAppearing`/`NoNav` flags, add semi-transparent background |
