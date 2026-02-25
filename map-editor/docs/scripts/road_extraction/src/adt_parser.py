"""
adt_parser.py — Parse WoW 3.3.5a ADT terrain files.

ADT files are monolithic (no _tex0/_obj0 split — that's Cataclysm+).
Chunk tags are stored reversed in file (MCNK → b'KNCM').

Parsing order: MVER → MHDR → MCIN → MTEX → MCNK[256]

MCNK header (128 bytes at offset 0x08 from MCNK tag):
  0x0C: nLayers (uint32)
  0x1C: ofsMCLY (uint32) — relative to MCNK tag start
  0x24: ofsMCAL (uint32) — relative to MCNK tag start
  0x28: sizeAlpha (uint32) — actual MCAL data size
  0x68: position[3] (float x3) — stored as (Y, Z, X) in WoW coords
"""

import struct
from dataclasses import dataclass, field

import numpy as np

from .mcal_decoder import decode_alpha


# ---------------------------------------------------------------------------
# Data classes
# ---------------------------------------------------------------------------

@dataclass
class MclyEntry:
    texture_id: int
    flags: int
    offset_in_mcal: int
    effect_id: int


@dataclass
class McnkData:
    index_x: int  # chunk column (0-15)
    index_y: int  # chunk row (0-15)
    position_x: float  # WoW world X (from header Y,Z,X → position[2])
    position_y: float  # WoW world Y (from header Y,Z,X → position[0])
    position_z: float  # WoW world Z (from header Y,Z,X → position[1])
    n_layers: int
    layers: list[MclyEntry] = field(default_factory=list)
    mcal_data: bytes = b""
    mcal_size: int = 0
    has_liquid: bool = False
    liquid_coverage: float = 0.0
    liquid_bitmap: np.ndarray | None = None  # 8x8 bool array, or None


@dataclass
class AdtData:
    mtex_list: list[str]  # texture path strings from MTEX
    chunks: list[McnkData]  # 256 MCNK chunks (row-major)


# ---------------------------------------------------------------------------
# Chunk finding
# ---------------------------------------------------------------------------

def _find_chunk(data: bytes, tag: bytes, start: int = 0) -> tuple[int, int] | None:
    """Find an IFF chunk by reversed tag. Returns (data_offset, data_size)."""
    pos = start
    while pos + 8 <= len(data):
        if data[pos:pos + 4] == tag:
            size = struct.unpack_from("<I", data, pos + 4)[0]
            return pos + 8, size
        chunk_size = struct.unpack_from("<I", data, pos + 4)[0]
        pos += 8 + chunk_size
    return None


def _find_subchunk(data: bytes, tag: bytes, mcnk_tag_pos: int, offset_from_tag: int) -> tuple[int, int] | None:
    """Find a sub-chunk within MCNK using the offset from MCNK tag start."""
    pos = mcnk_tag_pos + offset_from_tag
    if pos + 8 > len(data):
        return None
    found_tag = data[pos:pos + 4]
    size = struct.unpack_from("<I", data, pos + 4)[0]
    if found_tag == tag:
        return pos + 8, size
    return None


# ---------------------------------------------------------------------------
# MTEX parser
# ---------------------------------------------------------------------------

def _parse_mtex(data: bytes, offset: int, size: int) -> list[str]:
    """Parse MTEX chunk: null-separated texture path strings."""
    raw = data[offset:offset + size]
    # Split on null bytes, filter empty strings
    parts = raw.split(b"\x00")
    return [p.decode("utf-8", errors="replace") for p in parts if p]


# ---------------------------------------------------------------------------
# MH2O parser
# ---------------------------------------------------------------------------

def _parse_mh2o_for_chunks(data: bytes, mhdr_off: int, chunks: list[McnkData]) -> None:
    """Parse MH2O liquid headers and annotate McnkData with liquid info.

    MH2O is located via MHDR offset at byte 0x14 (ofsMH2O).
    Contains 256 SLiquidChunk entries (12 bytes each), one per MCNK.
    All offsets inside MH2O are relative to mh2o_data_start.
    """
    # MHDR offsets are relative to MHDR data start
    # MHDR layout: 0x00=flags, 0x04=mcin, 0x08=mtex, 0x0C=mmdx, 0x10=mmid,
    #   0x14=mwmo, 0x18=mwid, 0x1C=mddf, 0x20=modf, 0x24=mfbo, 0x28=mh2o
    ofs_mh2o = struct.unpack_from("<I", data, mhdr_off + 0x28)[0]
    if ofs_mh2o == 0:
        return

    # MH2O absolute position = MHDR data start + offset
    mh2o_abs = mhdr_off + ofs_mh2o

    if mh2o_abs + 8 > len(data):
        return

    # Check if there's a chunk tag here
    tag = data[mh2o_abs:mh2o_abs + 4]
    if tag == b"O2HM":
        mh2o_size = struct.unpack_from("<I", data, mh2o_abs + 4)[0]
        mh2o_data_start = mh2o_abs + 8
        mh2o_data_end = mh2o_data_start + mh2o_size
    else:
        mh2o_data_start = mh2o_abs
        mh2o_data_end = len(data)

    # Build chunk lookup by (index_x, index_y)
    chunk_map = {}
    for chunk in chunks:
        chunk_map[(chunk.index_x, chunk.index_y)] = chunk

    # 256 SLiquidChunk entries, 12 bytes each: {offset_instances, layer_count, offset_attributes}
    ENTRY_SIZE = 12
    header_end = mh2o_data_start + 256 * ENTRY_SIZE

    for idx in range(256):
        entry_off = mh2o_data_start + idx * ENTRY_SIZE
        if entry_off + ENTRY_SIZE > mh2o_data_end:
            break

        offset_instances, layer_count, offset_attributes = struct.unpack_from(
            "<III", data, entry_off
        )

        if layer_count == 0 or offset_instances == 0:
            continue

        cx = idx % 16
        cy = idx // 16
        chunk = chunk_map.get((cx, cy))
        if chunk is None:
            continue

        chunk.has_liquid = True

        # Parse first SLiquidInstance (24 bytes) — offsets relative to mh2o_data_start
        inst_abs = mh2o_data_start + offset_instances
        if inst_abs + 24 > len(data):
            chunk.liquid_coverage = 1.0
            continue

        (liquid_type, liquid_vertex_format,
         min_height, max_height,
         x_offset, y_offset, width, height,
         offset_exists_bitmap, offset_vertex_data) = struct.unpack_from(
            "<HHffBBBBII", data, inst_abs
        )

        # Build 8x8 liquid bitmap with bounds validation
        bitmap = np.zeros((8, 8), dtype=bool)

        # Clamp sub-rect to 8x8 grid
        if x_offset > 7 or y_offset > 7:
            bitmap[:] = True
        elif width == 0 or height == 0:
            bitmap[:] = True
        else:
            w = min(width, 8 - x_offset)
            h = min(height, 8 - y_offset)

            if offset_exists_bitmap != 0:
                bmp_abs = mh2o_data_start + offset_exists_bitmap
                total_bits = width * height
                total_bytes = (total_bits + 7) // 8

                if bmp_abs + total_bytes <= len(data):
                    bmp_data = data[bmp_abs:bmp_abs + total_bytes]
                    bit_idx = 0
                    for row in range(height):
                        for col in range(width):
                            byte_idx = bit_idx // 8
                            bit_pos = bit_idx % 8
                            if byte_idx < len(bmp_data) and (bmp_data[byte_idx] >> bit_pos) & 1:
                                br = y_offset + row
                                bc = x_offset + col
                                if 0 <= br < 8 and 0 <= bc < 8:
                                    bitmap[br, bc] = True
                            bit_idx += 1
                else:
                    bitmap[y_offset:y_offset + h, x_offset:x_offset + w] = True
            else:
                bitmap[y_offset:y_offset + h, x_offset:x_offset + w] = True

        chunk.liquid_bitmap = bitmap
        chunk.liquid_coverage = float(bitmap.sum()) / 64.0


# ---------------------------------------------------------------------------
# MCNK parser
# ---------------------------------------------------------------------------

def _parse_mcnk(data: bytes, mcnk_tag_pos: int, wdt_mphd_flags: int) -> McnkData:
    """Parse a single MCNK chunk at the given file position of its IFF tag."""
    # Header starts 8 bytes after tag (after tag + size fields)
    hdr_pos = mcnk_tag_pos + 8

    # Read header fields
    # 0x00: flags (u32)
    index_x = struct.unpack_from("<I", data, hdr_pos + 0x04)[0]  # ix
    index_y = struct.unpack_from("<I", data, hdr_pos + 0x08)[0]  # iy
    n_layers = struct.unpack_from("<I", data, hdr_pos + 0x0C)[0]

    ofs_mcly = struct.unpack_from("<I", data, hdr_pos + 0x1C)[0]  # relative to MCNK tag
    ofs_mcal = struct.unpack_from("<I", data, hdr_pos + 0x24)[0]  # relative to MCNK tag
    size_alpha = struct.unpack_from("<I", data, hdr_pos + 0x28)[0]

    # Position at offset 0x68: 3 floats.
    # Empirically verified: float[0]=wowX, float[1]=wowZ, float[2]=wowY
    # (Goldshire tile: float[0] range covers wowX=-9460, float[2] covers wowY=62)
    pos_x, pos_z, pos_y = struct.unpack_from("<fff", data, hdr_pos + 0x68)

    # Parse MCLY sub-chunk
    layers = []
    if n_layers > 0 and ofs_mcly > 0:
        mcly_pos = mcnk_tag_pos + ofs_mcly
        # Try to find the MCLY tag
        result = _find_subchunk(data, b"YLCM", mcnk_tag_pos, ofs_mcly)
        if result:
            mcly_data_pos, mcly_size = result
        else:
            # Fallback: assume data starts right at the offset + 8 (tag+size)
            mcly_data_pos = mcly_pos + 8
            mcly_size = n_layers * 16

        for i in range(min(n_layers, 4)):
            entry_pos = mcly_data_pos + i * 16
            if entry_pos + 16 > len(data):
                break
            tex_id, flags, offset_in_mcal, effect_id = struct.unpack_from("<IIIi", data, entry_pos)
            layers.append(MclyEntry(
                texture_id=tex_id,
                flags=flags,
                offset_in_mcal=offset_in_mcal,
                effect_id=effect_id,
            ))

    # Extract MCAL raw data
    mcal_data = b""
    if size_alpha > 0 and ofs_mcal > 0:
        mcal_pos = mcnk_tag_pos + ofs_mcal
        # Try to find the MCAL tag
        result = _find_subchunk(data, b"LACM", mcnk_tag_pos, ofs_mcal)
        if result:
            mcal_data_pos, mcal_chunk_size = result
        else:
            mcal_data_pos = mcal_pos + 8
            mcal_chunk_size = size_alpha

        mcal_data = data[mcal_data_pos:mcal_data_pos + size_alpha]

    return McnkData(
        index_x=index_x,
        index_y=index_y,
        position_x=pos_x,
        position_y=pos_y,
        position_z=pos_z,
        n_layers=n_layers,
        layers=layers,
        mcal_data=mcal_data,
        mcal_size=size_alpha,
    )


# ---------------------------------------------------------------------------
# Full ADT parser
# ---------------------------------------------------------------------------

def parse_adt(data: bytes, wdt_mphd_flags: int = 0) -> AdtData:
    """Parse a complete ADT file.

    Parameters:
        data: raw ADT file bytes
        wdt_mphd_flags: MPHD flags from WDT (needed for alpha format selection)
    """
    # MVER check
    result = _find_chunk(data, b"REVM")
    if result is None:
        raise ValueError("ADT: missing MVER chunk")
    ver_off, _ = result
    version = struct.unpack_from("<I", data, ver_off)[0]
    if version != 18:
        raise ValueError(f"ADT: unexpected version {version} (expected 18)")

    # MTEX
    result = _find_chunk(data, b"XETM")
    if result is None:
        raise ValueError("ADT: missing MTEX chunk")
    mtex_off, mtex_size = result
    mtex_list = _parse_mtex(data, mtex_off, mtex_size)

    # MCIN — 256 entries x 16 bytes, offsets are absolute from file start
    result = _find_chunk(data, b"NICM")
    if result is None:
        raise ValueError("ADT: missing MCIN chunk")
    mcin_off, mcin_size = result

    chunks = []
    for i in range(256):
        entry_pos = mcin_off + i * 16
        mcnk_abs_offset = struct.unpack_from("<I", data, entry_pos)[0]
        mcnk_size = struct.unpack_from("<I", data, entry_pos + 4)[0]

        if mcnk_abs_offset == 0:
            continue

        chunk = _parse_mcnk(data, mcnk_abs_offset, wdt_mphd_flags)
        chunks.append(chunk)

    # Parse MHDR for MH2O offset
    result = _find_chunk(data, b"RDHM")
    if result is not None:
        mhdr_off, mhdr_size = result
        _parse_mh2o_for_chunks(data, mhdr_off, chunks)

    return AdtData(mtex_list=mtex_list, chunks=chunks)


def decode_chunk_alphas(
    chunk: McnkData,
    wdt_mphd_flags: int,
) -> list[np.ndarray]:
    """Decode all alpha maps for a chunk's layers (skipping layer 0).

    Returns a list of 64x64 uint8 arrays, one per layer starting from layer 1.
    """
    alphas = []
    for i, layer in enumerate(chunk.layers):
        if i == 0:
            # Layer 0 has no alpha map (base texture, 100% opacity)
            continue
        if not (layer.flags & 0x100):
            # No alpha map for this layer
            continue

        # Determine end offset for RLE
        next_offset = None
        if i + 1 < len(chunk.layers):
            next_offset = chunk.layers[i + 1].offset_in_mcal
        else:
            next_offset = chunk.mcal_size

        alpha = decode_alpha(
            chunk.mcal_data,
            layer.offset_in_mcal,
            wdt_mphd_flags,
            layer.flags,
            next_offset=next_offset,
        )
        alphas.append(alpha)

    return alphas
