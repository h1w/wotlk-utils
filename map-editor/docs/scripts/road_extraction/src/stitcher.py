"""
stitcher.py — Stitch multiple 1024x1024 tile masks into a single large raster.

Handles correct world-coordinate positioning so roads are continuous
across ADT tile boundaries.
"""

import numpy as np
from PIL import Image

from .coord_utils import TILE_PIXELS


def stitch_tiles(
    tile_masks: dict[tuple[int, int], np.ndarray],
) -> tuple[np.ndarray, int, int, int, int]:
    """Stitch multiple 1024x1024 tile masks into a single raster.

    Parameters:
        tile_masks: dict of (tileX, tileY) → 1024x1024 bool array

    Returns:
        (stitched_raster, min_tile_x, min_tile_y, max_tile_x, max_tile_y)

    The raster is oriented with (0,0) at NW corner:
      row 0 = northernmost tile row, increasing rows go south
      col 0 = westernmost tile col, increasing cols go east

    Tile grid in ADT space: tileX increases south (decreasing wowX),
    tileY increases east (decreasing wowY). So:
      raster_row = (tileX - min_tileX) * 1024
      raster_col = (tileY - min_tileY) * 1024
    """
    if not tile_masks:
        return np.zeros((TILE_PIXELS, TILE_PIXELS), dtype=bool), 0, 0, 0, 0

    tile_xs = [k[0] for k in tile_masks]
    tile_ys = [k[1] for k in tile_masks]
    min_tx, max_tx = min(tile_xs), max(tile_xs)
    min_ty, max_ty = min(tile_ys), max(tile_ys)

    nx = max_tx - min_tx + 1
    ny = max_ty - min_ty + 1

    raster = np.zeros((nx * TILE_PIXELS, ny * TILE_PIXELS), dtype=bool)

    for (tx, ty), mask in tile_masks.items():
        r0 = (tx - min_tx) * TILE_PIXELS
        c0 = (ty - min_ty) * TILE_PIXELS
        raster[r0:r0 + TILE_PIXELS, c0:c0 + TILE_PIXELS] = mask

    return raster, min_tx, min_ty, max_tx, max_ty


def save_stitched_image(
    raster: np.ndarray,
    output_path: str,
    max_dim: int = 8192,
):
    """Save a stitched raster as a PNG image, downscaling if too large."""
    h, w = raster.shape
    scale = 1
    if max(h, w) > max_dim:
        scale = max(h, w) // max_dim + 1

    if scale > 1:
        # Downsample using max pooling (any road pixel → True)
        new_h = h // scale
        new_w = w // scale
        cropped = raster[:new_h * scale, :new_w * scale]
        reshaped = cropped.reshape(new_h, scale, new_w, scale)
        downsampled = reshaped.any(axis=(1, 3))
        img_data = downsampled.astype(np.uint8) * 255
    else:
        img_data = raster.astype(np.uint8) * 255

    img = Image.fromarray(img_data, mode="L")
    img.save(output_path)
    return scale
