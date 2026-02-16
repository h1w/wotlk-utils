"""
Verify the exact byte pattern that will be used in the C++ scanner.
This script mimics the C++ implementation for validation.

Pattern to detect:
1. Find: 0F B6 [ModRM] [disp32]  — movzx r32, byte ptr [r32 + REMAP_OFF]
   - ModRM: 0x80-0xBF, rm != 4 (no SIB)
2. Within 50 bytes: FF 24 [SIB] [disp32]  — jmp [r32*4 + JTABLE_OFF]
   - SIB: scale=2 (meaning *4), base=5 (disp32 only)
3. Before movzx: 3D [byte] 00 00 00 — cmp eax, MAX_TYPE (or 3C [byte] for cmp al)

Then:
- Read remap table (256 bytes) at REMAP_OFF from the binary
- Find default index (most frequent)
- Count unique non-default handler groups
- Report handler group → type byte aliases mapping
"""

import struct

def read_module(path):
    with open(path, 'rb') as f:
        return f.read()

def parse_header(data):
    fields = struct.unpack_from('<10I', data, 0)
    names = ['moduleSize', 'reserved', 'relocDataOff', 'relocCount',
             'exportTableOff', 'exportCount', 'baseIndex',
             'importTableOff', 'importLibCount', 'sectionDescCount']
    h = dict(zip(names, fields))
    h['packedDataOff'] = 0x28 + h['sectionDescCount'] * 12
    return h

def scan_for_remap_pattern(data, scan_start, scan_end):
    """
    C++-equivalent scan: find movzx + jmp pattern.
    Returns (remap_off, jtable_off, movzx_pos, jmp_pos, max_type) or None.
    """
    for i in range(scan_start, min(scan_end, len(data) - 7)):
        # Check for movzx: 0F B6 [ModRM] [disp32]
        if data[i] != 0x0F or data[i+1] != 0xB6:
            continue

        modrm = data[i+2]
        # ModRM must be mod=10 (0x80-0xBF), rm != 4 (no SIB)
        if not (0x80 <= modrm <= 0xBF) or (modrm & 7) == 4:
            continue

        remap_off = struct.unpack_from('<I', data, i+3)[0]
        # Remap table must be within the binary and at a reasonable offset
        if remap_off < 0x100 or remap_off + 256 > len(data):
            continue

        # Check for jmp: FF 24 [SIB] [disp32] within next 50 bytes
        for j in range(i+7, min(i+57, len(data) - 7)):
            if data[j] != 0xFF or data[j+1] != 0x24:
                continue

            sib = data[j+2]
            scale = sib >> 6
            base = sib & 7
            # scale must be 2 (*4) and base must be 5 (disp32 only, no base reg)
            if scale != 2 or base != 5:
                continue

            jtable_off = struct.unpack_from('<I', data, j+3)[0]

            # Look backwards for range check: cmp eax, imm32 (3D XX 00 00 00)
            max_type = 0xFF
            for k in range(i-1, max(scan_start, i-40), -1):
                if k + 5 <= len(data) and data[k] == 0x3D:
                    val = struct.unpack_from('<I', data, k+1)[0]
                    if 0x80 <= val <= 0xFF:
                        max_type = val
                        break
                # Also check: 3C XX (cmp al, imm8)
                if k + 2 <= len(data) and data[k] == 0x3C:
                    val = data[k+1]
                    if 0x80 <= val <= 0xFF:
                        max_type = val
                        break

            return {
                'remap_off': remap_off,
                'jtable_off': jtable_off,
                'movzx_pos': i,
                'jmp_pos': j,
                'max_type': max_type,
                'modrm': modrm,
                'sib': sib,
            }

    return None

def extract_handler_groups(data, remap_off, max_type):
    """Read remap table, find default index, group by handler."""
    table = data[remap_off:remap_off + max_type + 1]

    # Count occurrences of each index
    counts = {}
    for i in range(len(table)):
        idx = table[i]
        counts[idx] = counts.get(idx, 0) + 1

    # Default = most common
    default_idx = max(counts.keys(), key=lambda k: counts[k])

    # Group type bytes by handler
    groups = {}
    for i in range(len(table)):
        idx = table[i]
        if idx != default_idx:
            if idx not in groups:
                groups[idx] = []
            groups[idx].append(i)

    return table, groups, default_idx

def analyze_module(path, label, known_types=None):
    print(f"\n{'='*70}")
    print(f"Module: {label}")
    print(f"{'='*70}")

    data = read_module(path)
    header = parse_header(data)
    scan_start = header['packedDataOff']

    print(f"Size: {len(data)} bytes, scan from 0x{scan_start:04x}")

    result = scan_for_remap_pattern(data, scan_start, len(data))
    if not result:
        print("  NO REMAP+JUMP TABLE FOUND!")
        return

    print(f"\n  FOUND remap+jump table pattern!")
    print(f"  movzx at offset 0x{result['movzx_pos']:04x} (ModRM=0x{result['modrm']:02x})")
    print(f"  jmp   at offset 0x{result['jmp_pos']:04x} (SIB=0x{result['sib']:02x})")
    print(f"  Remap table at: 0x{result['remap_off']:04x}")
    print(f"  Jump table at:  0x{result['jtable_off']:04x}")
    print(f"  Max type value: 0x{result['max_type']:02x}")

    # Read remap table
    table, groups, default_idx = extract_handler_groups(
        data, result['remap_off'], result['max_type'])

    print(f"\n  Default handler index: {default_idx}")
    print(f"  Non-default handler groups: {len(groups)}")
    print(f"  Total non-default type bytes: {sum(len(v) for v in groups.values())}")

    # Show groups sorted by size (smallest first = most likely unique types)
    print(f"\n  Handler groups (smallest first):")
    for idx in sorted(groups.keys(), key=lambda k: len(groups[k])):
        types = groups[idx]
        known_str = ""
        if known_types:
            for t in types:
                if t in known_types:
                    known_str += f" [0x{t:02x}=KNOWN]"
        print(f"    group {idx:3d} ({len(types):2d} aliases): "
              f"{[f'0x{t:02x}' for t in sorted(types)]}{known_str}")

    # Verify known types if provided
    if known_types:
        print(f"\n  Known type verification:")
        type_to_group = {}
        for idx, types in groups.items():
            for t in types:
                type_to_group[t] = idx
        for t in sorted(known_types):
            if t in type_to_group:
                group_idx = type_to_group[t]
                aliases = groups[group_idx]
                print(f"    0x{t:02x} -> group {group_idx} "
                      f"({len(aliases)} aliases: {[f'0x{a:02x}' for a in sorted(aliases)]})")
            elif t <= result['max_type']:
                print(f"    0x{t:02x} -> DEFAULT (not a valid check type in remap table!)")
            else:
                print(f"    0x{t:02x} -> OUT OF RANGE (> max 0x{result['max_type']:02x})")

if __name__ == '__main__':
    base = r"C:\Users\bpqvg\source\repos\wotlk-utils\wotlk\docs\warden_dumps"

    known_7c = {0x1F, 0x22, 0x47, 0x69, 0x8E, 0x91, 0xB3, 0xD8, 0xDB}
    analyze_module(
        f"{base}\\warden_7C4ABC97B86494A2D91820785F3A1C87_decompressed.bin",
        "7C4ABC97",
        known_types=known_7c
    )

    analyze_module(
        f"{base}\\warden_9A95D19959AA88542116BE3639C0EB0F_decompressed.bin",
        "9A95D199"
    )
