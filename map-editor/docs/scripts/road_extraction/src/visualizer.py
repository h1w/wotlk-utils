"""
visualizer.py — Render road masks as images for visual verification.

Produces:
  - Binary mask (white on black)
  - Confidence color-coded (green=high, yellow=medium, red=low)
  - Raw alpha grayscale
"""

import numpy as np
from PIL import Image


def render_binary_mask(mask: np.ndarray) -> Image.Image:
    """Render a boolean mask as white-on-black image."""
    img = np.zeros(mask.shape, dtype=np.uint8)
    img[mask] = 255
    return Image.fromarray(img, mode="L")


def render_confidence_map(conf_map: np.ndarray) -> Image.Image:
    """Render confidence map as color-coded image.

    Green (>= 0.9): high confidence
    Yellow (0.5 - 0.9): medium confidence
    Red (> 0.0 and < 0.5): low confidence
    Black (0.0): no road
    """
    h, w = conf_map.shape
    rgb = np.zeros((h, w, 3), dtype=np.uint8)

    high = conf_map >= 0.9
    medium = (conf_map >= 0.5) & (conf_map < 0.9)
    low = (conf_map > 0.0) & (conf_map < 0.5)

    # Green for high confidence
    rgb[high] = [0, 255, 0]
    # Yellow for medium
    rgb[medium] = [255, 255, 0]
    # Red for low
    rgb[low] = [255, 0, 0]

    return Image.fromarray(rgb, mode="RGB")


def render_alpha_grayscale(
    adt_data,
    wdt_mphd_flags: int,
) -> Image.Image:
    """Render raw alpha values of ALL layers (not just roads) as grayscale.

    Composite: max alpha across all layers for each pixel.
    """
    from .mcal_decoder import decode_alpha

    composite = np.zeros((1024, 1024), dtype=np.uint8)

    for chunk in adt_data.chunks:
        chunk_row = chunk.index_y
        chunk_col = chunk.index_x

        for layer_idx in range(1, len(chunk.layers)):
            layer = chunk.layers[layer_idx]
            if not (layer.flags & 0x100):
                continue

            next_offset = None
            if layer_idx + 1 < len(chunk.layers):
                next_offset = chunk.layers[layer_idx + 1].offset_in_mcal
            else:
                next_offset = chunk.mcal_size

            try:
                alpha = decode_alpha(
                    chunk.mcal_data,
                    layer.offset_in_mcal,
                    wdt_mphd_flags,
                    layer.flags,
                    next_offset=next_offset,
                )
            except (ValueError, IndexError):
                continue

            r0 = chunk_row * 64
            c0 = chunk_col * 64
            composite[r0:r0 + 64, c0:c0 + 64] = np.maximum(
                composite[r0:r0 + 64, c0:c0 + 64], alpha
            )

    return Image.fromarray(composite, mode="L")


def save_tile_outputs(
    mask: np.ndarray,
    conf_map: np.ndarray | None,
    adt_data,
    wdt_mphd_flags: int,
    map_name: str,
    tile_x: int,
    tile_y: int,
    output_dir: str,
):
    """Save all visualization outputs for a single tile."""
    import os
    os.makedirs(output_dir, exist_ok=True)
    prefix = f"{map_name}_{tile_x}_{tile_y}"

    # Binary mask
    img = render_binary_mask(mask)
    img.save(os.path.join(output_dir, f"{prefix}_roads.png"))

    # Confidence map
    if conf_map is not None:
        img = render_confidence_map(conf_map)
        img.save(os.path.join(output_dir, f"{prefix}_confidence.png"))

    # Raw alpha
    img = render_alpha_grayscale(adt_data, wdt_mphd_flags)
    img.save(os.path.join(output_dir, f"{prefix}_alpha_raw.png"))
