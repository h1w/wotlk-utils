"""
road_mask.py — Build binary road masks from parsed ADT data.

Produces a 1024x1024 boolean mask per ADT tile (16 chunks x 64 pixels).
Each pixel is True if a road texture's alpha exceeds the threshold at that point.
"""

import numpy as np

from .adt_parser import AdtData, McnkData
from .mcal_decoder import decode_alpha
from .road_classifier import RoadConfig, is_road_layer


def build_road_mask(
    adt: AdtData,
    wdt_mphd_flags: int,
    threshold: int = 64,
    config: RoadConfig | None = None,
    dbc_data: dict | None = None,
    zone_hint: str = "",
) -> np.ndarray:
    """Build a 1024x1024 binary road mask for one ADT tile.

    Parameters:
        adt: parsed ADT data
        wdt_mphd_flags: MPHD flags from WDT
        threshold: alpha value cutoff (0-255), default 64 (25%)
        config: optional RoadConfig for zone overrides
        dbc_data: optional DBC data for supplementary classification
        zone_hint: zone name for config overrides

    Returns:
        1024x1024 boolean numpy array
    """
    mask = np.zeros((1024, 1024), dtype=bool)

    for chunk in adt.chunks:
        chunk_row = chunk.index_y  # row in tile grid
        chunk_col = chunk.index_x  # column in tile grid

        for layer_idx in range(1, len(chunk.layers)):
            layer = chunk.layers[layer_idx]

            # Skip layers without alpha maps
            if not (layer.flags & 0x100):
                continue

            # Check if texture is road
            if layer.texture_id >= len(adt.mtex_list):
                continue
            tex_path = adt.mtex_list[layer.texture_id]

            is_road, confidence = is_road_layer(
                tex_path,
                effect_id=layer.effect_id,
                dbc_data=dbc_data,
                config=config,
                zone_hint=zone_hint,
            )
            if not is_road:
                continue

            # Decode alpha for this layer
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

            # Apply threshold and OR into mask
            r0 = chunk_row * 64
            c0 = chunk_col * 64
            mask[r0:r0 + 64, c0:c0 + 64] |= (alpha > threshold)

    return mask


def build_confidence_map(
    adt: AdtData,
    wdt_mphd_flags: int,
    threshold: int = 64,
    config: RoadConfig | None = None,
    dbc_data: dict | None = None,
    zone_hint: str = "",
) -> np.ndarray:
    """Build a 1024x1024 confidence map for road classification.

    Returns float32 array where each pixel has the max confidence of any
    road layer that exceeds the alpha threshold at that point.
    """
    conf_map = np.zeros((1024, 1024), dtype=np.float32)

    for chunk in adt.chunks:
        chunk_row = chunk.index_y
        chunk_col = chunk.index_x

        for layer_idx in range(1, len(chunk.layers)):
            layer = chunk.layers[layer_idx]
            if not (layer.flags & 0x100):
                continue
            if layer.texture_id >= len(adt.mtex_list):
                continue

            tex_path = adt.mtex_list[layer.texture_id]
            is_road, confidence = is_road_layer(
                tex_path,
                effect_id=layer.effect_id,
                dbc_data=dbc_data,
                config=config,
                zone_hint=zone_hint,
            )
            if not is_road:
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
            road_pixels = alpha > threshold
            r = slice(r0, r0 + 64)
            c = slice(c0, c0 + 64)
            conf_map[r, c] = np.where(
                road_pixels,
                np.maximum(conf_map[r, c], confidence),
                conf_map[r, c],
            )

    return conf_map
