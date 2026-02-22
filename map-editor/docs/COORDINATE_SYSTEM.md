# Coordinate System Reference

## WoW World Coordinates

WoW 3.3.5a uses a **left-handed** coordinate system:
- **X+** = North
- **Y+** = West
- **Z+** = Up

The map editor displays a **north-up** 2D map.

## Screen Mapping

```
Screen right = East  = WoW -Y  (screenX increases as wowY decreases)
Screen down  = South = WoW -X  (screenY increases as wowX decreases)
```

### World-to-Screen Transform

```cpp
screenX = viewportCenterX + (cameraY - wowY) * zoom;
screenY = viewportCenterY + (cameraX - wowX) * zoom;
```

### Screen-to-World Transform

```cpp
wowX = cameraX - (screenY - viewportCenterY) / zoom;
wowY = cameraY - (screenX - viewportCenterX) / zoom;
```

## Tile Coordinates

WoW maps are divided into a 64x64 grid of ADT tiles, each `533.33333` yards.

### World-to-Tile

```cpp
tileX = 31 - floor(wowX / 533.33333)
tileY = 31 - floor(wowY / 533.33333)
```

### Tile-to-World Bounds

```cpp
wowX_min = (31 - tileX) * 533.33333   // south edge
wowX_max = (32 - tileX) * 533.33333   // north edge
wowY_min = (31 - tileY) * 533.33333   // west edge
wowY_max = (32 - tileY) * 533.33333   // east edge
```

Valid tile range: `[0, 63]` for both axes. Tile (31, 31) contains the world origin (0, 0).

## Detour Navmesh Coordinates

TrinityCore mmtile files use Detour's coordinate system, which differs from WoW:

```
Detour[0] = WoW Y    (east-west)
Detour[1] = WoW Z    (vertical)
Detour[2] = WoW X    (north-south)
```

**No negation** — just axis remapping. Confirmed by TC `PathGenerator::BuildPolyPath`.

## Minimap Tile Naming

WoW client minimap tiles in MPQ use a **swapped** convention from TrinityCore mmtiles:

| System | File naming | Meaning |
|--------|-------------|---------|
| TC mmtile | `{mapId:03d}{tileX:02d}{tileY:02d}.mmtile` | tileX from wowX, tileY from wowY |
| WoW minimap | `{MapName}\map{gx:02d}_{gy:02d}` | gx from wowY (column), gy from wowX (row) |

So when looking up minimap tiles, the tileX/tileY are **swapped**: `map{ty}_{tx}`.

### md5translate.trs

Minimap BLP filenames are resolved via `Textures\Minimap\md5translate.trs`:
- Key: `MapName\mapXX_YY` (case-insensitive, may include `.blp` extension)
- Value: MD5 hash string
- Resolved path: `Textures\Minimap\{hash}.blp`

### BLP UV Orientation

Minimap BLP textures store row 0 = north. With north-up screen mapping, **no UV flip** is needed:
- `uv_min = (0, 0)` (top-left of texture)
- `uv_max = (1, 1)` (bottom-right of texture)

## Constants

```cpp
static constexpr float TILE_SIZE = 533.33333f;   // yards per tile
static constexpr float MAP_SIZE  = 64 * 533.33333f;  // ~34133 yards total
```

## Facing / Heading

WoW facing angle: `facing = atan2(dy, dx)` where dx/dy are WoW deltas.
- 0 = South
- pi/2 = West
- pi = North
- 3*pi/2 = East

(Not used in the map editor, but documented for reference.)
