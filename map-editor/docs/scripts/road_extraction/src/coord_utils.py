"""
coord_utils.py — WoW coordinate constants and pixel-to-world mapping.

WoW coordinate system:
  X+ = north, Y+ = west, Z+ = up
  ADT tile grid: 64x64 tiles, each 533.33 yards
  MCNK chunk grid: 16x16 per tile, each 33.33 yards
  Alpha map: 64x64 pixels per chunk, each ~0.52 yards
"""

TILE_SIZE = 533.33333       # yards per ADT tile (= 1600/3)
CHUNK_SIZE = TILE_SIZE / 16  # 33.33333 yards per MCNK chunk (= 100/3)
PIXEL_SIZE = CHUNK_SIZE / 64  # ~0.520833 yards per alpha pixel (= 25/48)
MAP_ORIGIN = 32 * TILE_SIZE  # 17066.66666 (NW corner of full 64x64 grid)

# Tile mask resolution
PIXELS_PER_CHUNK = 64
CHUNKS_PER_TILE = 16
TILE_PIXELS = PIXELS_PER_CHUNK * CHUNKS_PER_TILE  # 1024


def pixel_to_world(
    mcnk_position_x: float, mcnk_position_y: float,
    pixel_row: int, pixel_col: int
) -> tuple[float, float]:
    """Convert alpha map pixel (row, col) to WoW world (X, Y).

    MCNK position is the NW corner of the chunk.
    Pixel (0,0) = NW corner. Row increases southward (decreasing X).
    Column increases eastward (decreasing Y).
    """
    world_x = mcnk_position_x - pixel_row * PIXEL_SIZE
    world_y = mcnk_position_y - pixel_col * PIXEL_SIZE
    return world_x, world_y


def tile_origin(file_a: int, file_b: int) -> tuple[float, float]:
    """Get the NW corner world coordinates for ADT tile {Name}_{A}_{B}.adt.

    ADT filename convention: first index (A) maps to wowY axis,
    second index (B) maps to wowX axis. Verified empirically:
      Azeroth_31_49.adt → wowX in [-9567,-9067], wowY in [0,533]
      formula: wowX_NW = (32 - B) * 533.33,  wowY_NW = (32 - A) * 533.33

    NW corner = maximum wowX, maximum wowY for the tile.
    """
    world_x = (32 - file_b) * TILE_SIZE
    world_y = (32 - file_a) * TILE_SIZE
    return world_x, world_y


def raster_pixel_to_world(
    origin_x: float, origin_y: float,
    pixel_row: int, pixel_col: int
) -> tuple[float, float]:
    """Convert a pixel in a tile-sized raster (1024x1024) to world coords.

    origin_x, origin_y = NW corner of the tile in world coords.
    """
    world_x = origin_x - pixel_row * PIXEL_SIZE
    world_y = origin_y - pixel_col * PIXEL_SIZE
    return world_x, world_y
