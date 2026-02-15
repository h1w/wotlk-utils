"""
Warden Module RLE Unpacker

Unpacks a decompressed Warden module binary (_decompressed.bin) into a
runtime-ready memory image. The decompressed binary uses a custom Blizzard
format with a 40-byte header, section descriptors, and RLE-packed sections.

RLE format: alternating COPY/SKIP entries starting with COPY.
  - COPY: uint16 LE length + `length` literal bytes
  - SKIP: uint16 LE length (destination advances, zero-filled)

Usage:
    python unpack_rle.py <decompressed.bin> [--output <out.bin>] [--info] [--all <dumps_dir>]

Examples:
    python unpack_rle.py warden_7C4ABC97..._decompressed.bin --info
    python unpack_rle.py warden_7C4ABC97..._decompressed.bin -o unpacked.bin
    python unpack_rle.py --all "Z:/Games/wow 3.3.5a client/warden_dumps"
"""

import struct
import sys
import os
import argparse
from pathlib import Path


def parse_header(data: bytes) -> dict:
    """Parse the 40-byte Warden module header."""
    if len(data) < 0x28:
        raise ValueError(f"Data too small for header: {len(data)} bytes")

    fields = struct.unpack_from('<10I', data, 0)
    return {
        'moduleSize':       fields[0],
        'reserved':         fields[1],
        'relocOff':         fields[2],
        'relocCount':       fields[3],
        'exportTableOff':   fields[4],
        'exportCount':      fields[5],
        'baseIndex':        fields[6],
        'importTableOff':   fields[7],
        'importLibCount':   fields[8],
        'sectionDescCount': fields[9],
    }


def parse_sections(data: bytes, section_count: int) -> list:
    """Parse section descriptors (12 bytes each, at offset 0x28)."""
    sections = []
    for i in range(section_count):
        off = 0x28 + i * 12
        vaddr, vsize, flags = struct.unpack_from('<3I', data, off)
        sections.append({
            'virtualAddr': vaddr,
            'virtualSize': vsize,
            'flags':       flags,
        })
    return sections


FLAG_NAMES = {0x20: 'CODE', 0x02: 'IDATA', 0x04: 'BSS'}


def unpack_rle(data: bytes) -> bytes:
    """
    Unpack a decompressed Warden module binary into a runtime memory image.

    Returns the unpacked image as bytes.
    """
    header = parse_header(data)
    module_size = header['moduleSize']
    section_count = header['sectionDescCount']
    sections = parse_sections(data, section_count)

    # Allocate runtime image (zero-filled)
    image = bytearray(module_size)

    # Copy 40-byte header verbatim
    image[0:0x28] = data[0:0x28]

    # RLE stream starts after section descriptors
    src_pos = 0x28 + section_count * 12
    # Destination starts at first section's virtualAddr
    dest_pos = sections[0]['virtualAddr'] if sections else 0x1000

    is_skip = False  # first entry is always COPY
    steps = 0

    while dest_pos < module_size:
        if src_pos + 2 > len(data):
            raise ValueError(
                f"RLE source exhausted at srcPos={src_pos} "
                f"(file size={len(data)}, destPos={dest_pos}/{module_size})")

        length = struct.unpack_from('<H', data, src_pos)[0]
        src_pos += 2

        if not is_skip:
            # COPY: literal bytes
            if src_pos + length > len(data):
                raise ValueError(
                    f"COPY overruns source at step {steps}: "
                    f"srcPos={src_pos}, length={length}, file size={len(data)}")
            if dest_pos + length > module_size:
                raise ValueError(
                    f"COPY overruns image at step {steps}: "
                    f"destPos={dest_pos}, length={length}, imageSize={module_size}")
            image[dest_pos:dest_pos + length] = data[src_pos:src_pos + length]
            src_pos += length
        # SKIP: dest advances (image already zeroed)

        dest_pos += length
        is_skip = not is_skip
        steps += 1

    remainder = len(data) - src_pos
    return bytes(image), {
        'steps': steps,
        'src_consumed': src_pos,
        'remainder': remainder,
        'module_size': module_size,
    }


def print_info(data: bytes, filename: str = ""):
    """Print detailed module information."""
    header = parse_header(data)
    sections = parse_sections(data, header['sectionDescCount'])

    name = os.path.basename(filename) if filename else "module"
    print(f"=== {name} ===")
    print(f"  File size:        {len(data)} bytes")
    print(f"  Runtime size:     {header['moduleSize']} (0x{header['moduleSize']:X})")
    print(f"  Reloc offset:     0x{header['relocOff']:X} ({header['relocCount']} entries)")
    print(f"  Export table:     0x{header['exportTableOff']:X} ({header['exportCount']} exports)")
    print(f"  Import table:     0x{header['importTableOff']:X} ({header['importLibCount']} libs)")
    print(f"  Base index:       {header['baseIndex']}")
    print(f"  Sections:         {header['sectionDescCount']}")

    packed_start = 0x28 + header['sectionDescCount'] * 12
    total_vsize = sum(s['virtualSize'] for s in sections)
    print(f"  Packed data at:   0x{packed_start:X}")
    print(f"  Total vsize:      {total_vsize} bytes")

    for i, s in enumerate(sections):
        flag_str = FLAG_NAMES.get(s['flags'], f'0x{s["flags"]:02X}')
        print(f"    [{i}] vaddr=0x{s['virtualAddr']:04X}  "
              f"vsize=0x{s['virtualSize']:04X} ({s['virtualSize']:5d})  "
              f"flags={flag_str}")

    # Unpack and report
    image, stats = unpack_rle(data)
    print(f"  RLE steps:        {stats['steps']}")
    print(f"  Source consumed:   {stats['src_consumed']}/{len(data)} "
          f"(remainder={stats['remainder']})")
    ratio = len(data) / header['moduleSize'] * 100
    print(f"  Compression:      {ratio:.1f}%")

    return image


def process_all(dumps_dir: str):
    """Process all _decompressed.bin files in a directory."""
    p = Path(dumps_dir)
    files = sorted(p.glob('*_decompressed.bin'))
    if not files:
        print(f"No _decompressed.bin files found in {dumps_dir}")
        return

    print(f"Found {len(files)} module(s)\n")
    ok = 0
    fail = 0
    for f in files:
        try:
            data = f.read_bytes()
            print_info(data, str(f))
            ok += 1
        except Exception as e:
            print(f"  ERROR: {e}")
            fail += 1
        print()

    print(f"Summary: {ok} OK, {fail} failed out of {len(files)}")


def main():
    parser = argparse.ArgumentParser(description='Warden Module RLE Unpacker')
    parser.add_argument('input', nargs='?', help='Input _decompressed.bin file')
    parser.add_argument('-o', '--output', help='Output unpacked image file')
    parser.add_argument('--info', action='store_true', help='Print module info')
    parser.add_argument('--all', metavar='DIR', help='Process all modules in directory')

    args = parser.parse_args()

    if args.all:
        process_all(args.all)
        return

    if not args.input:
        parser.print_help()
        return

    data = Path(args.input).read_bytes()

    if args.info or not args.output:
        print_info(data, args.input)

    if args.output:
        image, stats = unpack_rle(data)
        Path(args.output).write_bytes(image)
        print(f"Written {len(image)} bytes to {args.output}")


if __name__ == '__main__':
    main()
