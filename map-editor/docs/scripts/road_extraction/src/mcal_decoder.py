"""
mcal_decoder.py — Decode MCAL alpha maps in all three WoW 3.3.5a formats.

Format selection:
  - MCLY flags & 0x200 → RLE compressed (variable → 4096 bytes → 64x64)
  - MPHD flags & 0x004 → 8-bit uncompressed (4096 bytes → 64x64)
  - else              → 4-bit packed (2048 bytes → 64x64)
"""

import numpy as np


def get_alpha_format(wdt_mphd_flags: int, mcly_flags: int) -> str:
    """Determine the alpha decode format for a layer."""
    if mcly_flags & 0x200:
        return "compressed_rle"
    elif wdt_mphd_flags & 0x004:
        return "uncompressed_8bit"
    else:
        return "uncompressed_4bit"


def decode_4bit_alpha(data: bytes) -> np.ndarray:
    """Decode 4-bit packed alpha (2048 bytes → 64x64 uint8).

    Each byte has two 4-bit values, low nibble first.
    Values 0-15 are expanded to 0-255 via val | (val << 4).
    """
    if len(data) < 2048:
        raise ValueError(f"4-bit alpha needs 2048 bytes, got {len(data)}")

    raw = np.frombuffer(data[:2048], dtype=np.uint8)

    # Extract low and high nibbles
    low = raw & 0x0F
    high = (raw >> 4) & 0x0F

    # Interleave: low nibble first, then high nibble
    alpha = np.empty(4096, dtype=np.uint8)
    alpha[0::2] = low
    alpha[1::2] = high

    # Expand 0-15 → 0-255
    alpha = alpha | (alpha << 4)

    return alpha.reshape(64, 64)


def decode_8bit_alpha(data: bytes) -> np.ndarray:
    """Decode 8-bit uncompressed alpha (4096 bytes → 64x64 uint8)."""
    if len(data) < 4096:
        raise ValueError(f"8-bit alpha needs 4096 bytes, got {len(data)}")
    return np.frombuffer(data[:4096], dtype=np.uint8).reshape(64, 64).copy()


def decompress_alpha_rle(data: bytes) -> np.ndarray:
    """Decompress RLE alpha (variable → 4096 bytes → 64x64 uint8).

    Control byte: high bit = fill/copy mode, lower 7 bits = count.
      bit 7 set: FILL mode — repeat next byte 'count' times
      bit 7 clear: COPY mode — copy 'count' literal bytes
    Always produces exactly 4096 output bytes (capped, as Noggit does).
    """
    output = bytearray(4096)
    in_pos = 0
    out_pos = 0

    while out_pos < 4096 and in_pos < len(data):
        control = data[in_pos]
        in_pos += 1
        count = control & 0x7F

        if control & 0x80:
            # FILL mode
            if in_pos >= len(data):
                break
            value = data[in_pos]
            in_pos += 1
            end = min(out_pos + count, 4096)
            for i in range(out_pos, end):
                output[i] = value
            out_pos = end
        else:
            # COPY mode
            for _ in range(count):
                if out_pos >= 4096 or in_pos >= len(data):
                    break
                output[out_pos] = data[in_pos]
                in_pos += 1
                out_pos += 1

    return np.frombuffer(bytes(output), dtype=np.uint8).reshape(64, 64)


def decode_alpha(
    mcal_data: bytes,
    offset_in_mcal: int,
    wdt_mphd_flags: int,
    mcly_flags: int,
    next_offset: int | None = None,
) -> np.ndarray:
    """Decode a single layer's alpha map from MCAL data.

    Parameters:
        mcal_data: raw MCAL chunk data
        offset_in_mcal: byte offset of this layer within MCAL data
        wdt_mphd_flags: MPHD flags from WDT
        mcly_flags: flags from this layer's MCLY entry
        next_offset: byte offset of the next layer (for RLE size calc), or None
    """
    fmt = get_alpha_format(wdt_mphd_flags, mcly_flags)
    start = offset_in_mcal

    if fmt == "compressed_rle":
        # RLE: variable-length data from start to next_offset or end of MCAL
        end = next_offset if next_offset is not None else len(mcal_data)
        return decompress_alpha_rle(mcal_data[start:end])
    elif fmt == "uncompressed_8bit":
        return decode_8bit_alpha(mcal_data[start:start + 4096])
    else:
        return decode_4bit_alpha(mcal_data[start:start + 2048])
