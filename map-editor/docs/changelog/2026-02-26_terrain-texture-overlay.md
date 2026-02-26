# Terrain Texture Overlay (TASK-010)

**Date:** 2026-02-26
**Task:** TASK-010 (all 5 phases)

## Summary

Added game texture rendering to the map editor's 3D terrain view. Previously terrain rendered as procedural color (solid grey / height gradient / slope shading). Now the actual WoW ground textures -- grass, dirt, roads, stone -- can be overlaid on the terrain mesh. The implementation reads ADT texture layers from MPQ archives, composites up to 4 layers per MCNK chunk on the CPU into a 1024x1024 BGRA atlas per tile, and renders via a new textured DX11 pipeline. Toggle-able via "Textures" checkbox in the Layers panel (default OFF). Tiles without ADT data (ocean) gracefully fall back to procedural color.

## Architecture

### Data Flow

```
WDT (MPQ) --> MPHD flags (alpha format: 4-bit / 8-bit / RLE)
ADT (MPQ) --> MTEX paths + MCLY layers + MCAL raw alpha data
BLP (MPQ) --> DecodeBlp() --> BGRA pixels (cached in BlpTextureCache)
                |
                v
    TerrainTextureCompositor (CPU, worker thread)
    Per MCNK chunk (256 per tile):
      1. Decode alpha map (4-bit packed / 8-bit / RLE)
      2. Tile BLP texture across 64x64 output (~8 repeats)
      3. Alpha-blend layers 0..3 --> single BGRA block
                |
                v
    1024x1024 BGRA atlas (16x16 grid of 64x64 blocks)
                |
                v
    GPU: Texture2D + SRV (main thread upload)
                |
                v
    TerrainTexturePipeline (DX11 shader, colorMode==3)
    VS: transform position, pass UV
    PS: sample atlas, apply lighting
```

### Key Design Decisions

- **Separate `TerrainTexturePipeline`**: Created a new pipeline with 32-byte vertex layout (POSITION + NORMAL + TEXCOORD) instead of modifying the existing `TerrainPipeline`. This avoids breaking `BuildingRenderer`, which shares `TerrainPipeline` with its own 24-byte vertices.
- **colorMode==3**: Reused the existing `heightParams.z` constant buffer field to signal textured mode in the pixel shader, avoiding constant buffer layout changes.
- **Thread-safe BLP cache**: `BlpTextureCache` uses a mutex and caches failed loads as sentinel entries (width==0) to avoid repeated MPQ reads for missing textures.
- **LOD-gated texture loading**: Only tiles within distance threshold (9.0f) get textures; far tiles use procedural color.
- **Stale cache re-queuing**: When textures are toggled on, cached tiles without textures are re-queued for loading.

## Changes

### Phase 1: C++ ADT Texture Parser

**New files**: `adt_texture_parser.h/.cpp`, `mcal_decoder.h/.cpp`

- Ported Python ADT parser (`adt_parser.py`) to C++ for reading MTEX, MCLY, MCAL from ADT files
- IFF chunk navigation with correct little-endian tag encoding via `MakeTag()` helper
- WDT MPHD flags reader with per-map caching
- Map.dbc parsing via `DbcReader` for mapId-to-InternalName resolution
- MCAL alpha decoder supporting three formats:
  - 4-bit packed (2048 bytes, low nibble first, expanded 0-15 to 0-255)
  - 8-bit uncompressed (4096 bytes, direct copy)
  - RLE compressed (variable length, control byte with fill/copy modes)

### Phase 2: CPU Texture Compositor

**New files**: `terrain_texture_compositor.h/.cpp`

- `BlpTextureCache`: thread-safe cache for decoded BLP textures from MPQ
- `CompositeChunk()`: blends up to 4 texture layers into a 64x64 BGRA image per chunk
  - Layer 0 at full opacity (base texture)
  - Layers 1-3 alpha-blended using decoded MCAL alpha maps
  - BLP textures tiled ~8x per chunk to match WoW's ground texture density
- `CompositeTileAtlas()`: assembles 256 chunk composites into a 1024x1024 BGRA atlas

### Phase 3: UV Coordinates + Textured Pipeline

**Modified**: `terrain_mesh.h`, `terrain_mesh.cpp`, `terrain_pipeline.h`
**New files**: `terrain_texture_pipeline.h/.cpp`

- Extended `TerrainVertex` with `float u, v` (24 to 32 bytes)
- UV computed in `GenerateTerrainMesh()` for both V9 and V8 vertices:
  - `u = (tileOriginY - v.y) / TILE_SIZE` (0=west, 1=east)
  - `v = (tileOriginX - v.x) / TILE_SIZE` (0=north, 1=south)
- New `TerrainTexturePipeline` with:
  - 32-byte input layout (POSITION + NORMAL + TEXCOORD)
  - VS passes UV to PS
  - PS samples atlas texture when `colorMode==3`, falls back to procedural color otherwise
  - Bilinear sampler with clamp addressing

### Phase 4: Renderer Integration

**Modified**: `terrain_renderer.h`, `terrain_renderer.cpp`, `app.cpp`

- Extended `TileGpu` with `ID3D11Texture2D*`, `ID3D11ShaderResourceView*`, `bool hasTexture`
- Extended `LoadRequest` / `LoadResult` with texture data fields
- Worker thread: after mesh generation, parses ADT and composites atlas (guarded by `m_texMutex`)
- GPU upload: creates immutable `DXGI_FORMAT_B8G8R8A8_UNORM` texture from atlas
- Render: per-tile selects textured path (binds SRV + sampler, sets colorMode=3) or procedural fallback
- Stale cache handling: tiles loaded without textures are re-queued when textures become enabled
- `TerrainTexturePipeline` used for all terrain rendering (both textured and procedural modes)

### Phase 5: UI Toggle + Settings

**Modified**: `graph_renderer.h`, `layer_panel.cpp`, `app_settings.h`, `app_settings.cpp`, `app.cpp`

- Added `showTerrainTextures` to `LayerVisibility` (default OFF)
- Added "Textures" checkbox in Layers panel with tooltip explaining MPQ requirement
- Added `showTerrainTextures` to `AppSettings` with JSON persistence (`terrain_textures` key)
- Wired save/load in `app.cpp`

## New Files

| File | Purpose |
|------|---------|
| `src/data/adt_texture_parser.h` | ADT texture parsing (MTEX, MCLY, MCAL, WDT MPHD) |
| `src/data/adt_texture_parser.cpp` | Implementation |
| `src/data/mcal_decoder.h` | Alpha map decoder (4-bit, 8-bit, RLE) |
| `src/data/mcal_decoder.cpp` | Implementation |
| `src/data/terrain_texture_compositor.h` | CPU compositing (layers to atlas) + BLP cache |
| `src/data/terrain_texture_compositor.cpp` | Implementation |
| `src/render/terrain_texture_pipeline.h` | DX11 textured terrain shaders + pipeline |
| `src/render/terrain_texture_pipeline.cpp` | Implementation |

## Modified Files

| File | Changes |
|------|---------|
| `src/data/terrain_mesh.h` | Added `float u, v` to `TerrainVertex` (24 to 32 bytes) |
| `src/data/terrain_mesh.cpp` | UV coordinate generation in `GenerateTerrainMesh()` |
| `src/render/terrain_pipeline.h` | Added `float u, v` to `TerrainVertexGpu` |
| `src/render/terrain_renderer.h` | MPQ pointer, texture cache, AdtTextureParser, TerrainTexturePipeline, TileGpu tex/srv |
| `src/render/terrain_renderer.cpp` | Worker ADT load + composite, GPU texture upload, render path selection, stale cache re-queue |
| `src/render/graph_renderer.h` | `showTerrainTextures` in `LayerVisibility` |
| `src/ui/layer_panel.cpp` | "Textures" checkbox with tooltip |
| `src/data/app_settings.h` | `showTerrainTextures` field |
| `src/data/app_settings.cpp` | Load/save `terrain_textures` |
| `src/app.cpp` | Wire MPQ, map name, texture toggle to renderer |
| `map-editor.vcxproj` | Added 8 new source/header files |

## Bug Fixes During Implementation

### IFF Tag Byte Order (Critical)

Tag constants initially used big-endian shift order (`'R' << 24 | 'E' << 16 | ...`), producing `0x5245564D`. But `memcpy` on x86 reads little-endian, expecting `0x4D564552`. All `FindChunk` calls silently failed, returning no MVER/MTEX/MCIN data.

**Fix**: Replaced all tag constants with `MakeTag(c0,c1,c2,c3)` helper:
```cpp
static constexpr uint32_t MakeTag(char c0, char c1, char c2, char c3) {
    return static_cast<uint32_t>(static_cast<uint8_t>(c0))
         | (static_cast<uint32_t>(static_cast<uint8_t>(c1)) << 8)
         | (static_cast<uint32_t>(static_cast<uint8_t>(c2)) << 16)
         | (static_cast<uint32_t>(static_cast<uint8_t>(c3)) << 24);
}
```

### Stale GPU Cache

Tiles loaded before textures were enabled had `hasTexture=false` and were never re-queued (LOD check caused `continue`). Added `needsTexture` flag to bypass the LOD skip and force re-loading with textures.
