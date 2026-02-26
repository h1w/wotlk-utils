# Issue Overlay: Gap Distance Filter + 2D/3D Toggle + 3D Rendering + State Persistence

**Date:** 2026-02-26

## Summary

Three improvements to the Graph Issues system:

1. **Gap distance slider now filters the visual overlay** — previously the `m_gapMaxDistance` slider only filtered the issue *list* in the panel but had no effect on the 2D canvas rendering (dashed gap lines, component coloring were always drawn regardless of slider position). Now the slider value is passed to `RenderIssueOverlay()` and gaps exceeding the threshold are skipped.

2. **"Show overlay" toggle** — new checkbox in the issue panel header row allows toggling all issue visuals (component coloring + gap dashed lines) on/off in both 2D and 3D modes.

3. **3D issue overlay** — the issue overlay (component-colored edges/nodes, orange dashed gap lines) now renders in 3D mode via `Graph3DRenderer::RenderIssueOverlay()`, matching the 2D appearance.

4. **Full state persistence** — all UI state in the Graph Issues panel (graph mode, view mode, panel open/closed, gap distance slider, overlay toggle, all 5 type filter buttons) is now saved to `map_editor_settings.json` and restored on startup.

## Changes

### Gap Distance Filter for 2D Overlay

**`src/render/graph_renderer.h`** — Added `float gapMaxDistance` parameter to `RenderIssueOverlay`.

**`src/render/graph_renderer.cpp`** — Added early `continue` in the gap markers loop when `gap.distance > gapMaxDistance`.

**`src/app.cpp`** — 2D call site now passes `m_issuePanel.GetGapMaxDistance()` to the renderer.

### Overlay Toggle

**`src/ui/issue_panel.h`** — Added `bool m_showIssueOverlay = true` member + `GetShowIssueOverlay()` getter.

**`src/ui/issue_panel.cpp`** — Added `ImGui::Checkbox("Show overlay", &m_showIssueOverlay)` on the map info header row (after total count, before separator).

**`src/app.cpp`** — Both 2D and 3D issue overlay calls gated on `m_issuePanel.GetShowIssueOverlay()`.

### 3D Issue Overlay

**`src/render/graph_renderer_3d.h`** — Added `RenderIssueOverlay()` method declaration + forward declaration for `ValidationResult`.

**`src/render/graph_renderer_3d.cpp`** — Implementation:
- **Component-colored edges**: iterate edges, look up component from `validation.nodeComponent`, skip component 0 (largest). Draw via `prims.AddLine()` at z+0.7f elevation with per-component palette color (alpha=120).
- **Component-colored node rings**: iterate nodes, skip component 0. Draw `prims.AddCircle()` at z+0.5f, radius 3.5f, per-component color (alpha=220).
- **Gap dashed lines**: iterate `validation.gaps`, skip if `gap.distance > gapMaxDistance`. `DrawDashedLine3D()` helper breaks line into dash segments (4yd dash, 3yd gap). Orange color IM_COL32(255, 100, 30, 200) matches 2D.
- **ComponentColor3D**: duplicated 10-color palette function from `graph_renderer.cpp` (small static helper, not worth a shared header).

**`src/app.cpp`** — Added 3D call site after graph rendering block, before routes.

### State Persistence

**`src/ui/issue_panel.h`** — Added getters/setters for all persisted UI state:
- `SetGraphMode()` / `GetGraphMode()`
- `SetViewMode()` / `GetViewMode()`
- `IsPopupOpen()` / `SetPopupOpen()`
- `SetGapMaxDistance()` / `GetGapMaxDistance()`
- `SetShowIssueOverlay()` / `GetShowIssueOverlay()`
- `Get/SetShowDisconnected()`, `Get/SetShowDeadEnds()`, `Get/SetShowOrphans()`, `Get/SetShowDuplicates()`, `Get/SetShowZeroLength()`

**`src/data/app_settings.h`** — Added 10 `issue*` fields (graph mode, view mode, panel open, gap max distance, overlay toggle, 5 type filter bools).

**`src/data/app_settings.cpp`** — Load/save `"issue_panel"` JSON object with keys: `graph_mode`, `view_mode`, `panel_open`, `gap_max_distance`, `show_overlay`, `show_disconnected`, `show_dead_ends`, `show_orphans`, `show_duplicates`, `show_zero_length`.

**`src/app.cpp`** — `LoadSettings()` restores all issue panel state from `m_settings`; `SaveSettings()` reads all state from `m_issuePanel` into `m_settings`.

## Modified Files

| File | Changes |
|------|---------|
| `src/ui/issue_panel.h` | `m_showIssueOverlay` member, getters/setters for all UI state |
| `src/ui/issue_panel.cpp` | "Show overlay" checkbox in header row |
| `src/render/graph_renderer.h` | `float gapMaxDistance` param on `RenderIssueOverlay` |
| `src/render/graph_renderer.cpp` | Gap distance filter in rendering loop |
| `src/render/graph_renderer_3d.h` | `RenderIssueOverlay()` declaration, `ValidationResult` fwd decl |
| `src/render/graph_renderer_3d.cpp` | 3D component coloring + gap dashed lines + `ComponentColor3D` + `DrawDashedLine3D` |
| `src/data/app_settings.h` | 10 `issue*` settings fields |
| `src/data/app_settings.cpp` | Load/save `"issue_panel"` JSON object |
| `src/app.cpp` | 2D overlay gated + gapMaxDistance param, 3D overlay call, issue panel state save/load |

## Settings JSON Format

```json
{
  "issue_panel": {
    "graph_mode": 0,
    "view_mode": 0,
    "panel_open": false,
    "gap_max_distance": 20.0,
    "show_overlay": true,
    "show_disconnected": true,
    "show_dead_ends": true,
    "show_orphans": true,
    "show_duplicates": true,
    "show_zero_length": true
  }
}
```
