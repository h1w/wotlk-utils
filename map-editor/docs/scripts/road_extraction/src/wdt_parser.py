"""
wdt_parser.py — Parse WDT files to extract MPHD flags and tile existence grid.

WDT determines alpha map format for an entire map:
  - MPHD flag 0x4 (adt_has_big_alpha) → 8-bit alpha (4096 bytes)
  - No flag → 4-bit packed alpha (2048 bytes)

Chunk tags are stored reversed in file (MPHD → b'DHPM').
"""

import struct
from dataclasses import dataclass

import numpy as np


@dataclass
class WdtInfo:
    mphd_flags: int
    tile_exists: np.ndarray  # bool[64][64]

    @property
    def big_alpha(self) -> bool:
        return bool(self.mphd_flags & 0x4)


def _find_chunk(data: bytes, tag: bytes, start: int = 0) -> tuple[int, int] | None:
    """Find an IFF chunk by its reversed tag. Returns (data_offset, data_size) or None."""
    pos = start
    while pos + 8 <= len(data):
        chunk_tag = data[pos:pos + 4]
        chunk_size = struct.unpack_from("<I", data, pos + 4)[0]
        if chunk_tag == tag:
            return pos + 8, chunk_size
        pos += 8 + chunk_size
    return None


def parse_wdt(data: bytes) -> WdtInfo:
    """Parse a WDT file and extract MPHD flags and tile existence grid."""
    # MVER check
    result = _find_chunk(data, b"REVM")
    if result is None:
        raise ValueError("WDT: missing MVER chunk")
    ver_off, ver_size = result
    version = struct.unpack_from("<I", data, ver_off)[0]
    if version != 18:
        raise ValueError(f"WDT: unexpected version {version} (expected 18)")

    # MPHD — flags at offset 0 within chunk data
    result = _find_chunk(data, b"DHPM")
    if result is None:
        raise ValueError("WDT: missing MPHD chunk")
    mphd_off, mphd_size = result
    mphd_flags = struct.unpack_from("<I", data, mphd_off)[0]

    # MAIN — 64x64 entries, each 8 bytes: flags(u32) + asyncId(u32)
    # A tile exists if flags != 0
    result = _find_chunk(data, b"NIAM")
    if result is None:
        raise ValueError("WDT: missing MAIN chunk")
    main_off, main_size = result

    tile_exists = np.zeros((64, 64), dtype=bool)
    for y in range(64):
        for x in range(64):
            entry_off = main_off + (y * 64 + x) * 8
            flags = struct.unpack_from("<I", data, entry_off)[0]
            if flags & 1:
                tile_exists[y, x] = True

    return WdtInfo(mphd_flags=mphd_flags, tile_exists=tile_exists)
