# Persist View Mode & Performance Settings

**Date:** 2026-02-24

## Summary

View mode (2D/3D), VSync, FPS limit, and profiler overlay visibility now persist across application restarts via `map_editor_settings.json`.

## Problem

The FPS limiter, VSync toggle, profiler overlay checkbox, and 2D/3D mode selection were reset to defaults every time the application was restarted. Users had to reconfigure these settings on each launch.

## Solution

Added four new fields to `AppSettings` and wired them into the existing save/load pipeline:

| JSON key | Type | Default | Maps to |
|----------|------|---------|---------|
| `view_mode` | int | `0` (2D) | `App::m_viewMode` |
| `vsync` | bool | `false` | `App::m_vsync` |
| `fps_limit` | int | `0` (unlimited) | `App::m_fpsLimit` |
| `show_profiler` | bool | `true` | `App::m_showProfiler` |

## Modified Files

| File | Change |
|------|--------|
| `src/data/app_settings.h` | Added `viewMode`, `vsync`, `fpsLimit`, `showProfiler` members |
| `src/data/app_settings.cpp` | Serialize/deserialize new fields in `Load()` and `Save()` |
| `src/app.cpp` | Restore settings in `Initialize()`, sync to `m_settings` in `SaveSettings()` |
