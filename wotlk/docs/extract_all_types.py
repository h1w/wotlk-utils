"""
Extract check type IDs from ALL Warden modules via remap table analysis.

For EACH module:
1. Find remap+jump table (raw byte scan)
2. Read remap table → group entries by handler index
3. Find default handler (most common index)
4. Non-default groups = handler groups with real check types
5. Cross-validate with known types

Also checks if modules have MULTIPLE remap tables (request parser + response builder).
"""

import struct
import zlib
import os

DUMP_DIR = r"Z:\Games\wow 3.3.5a client\warden_dumps"
DOCS_DIR = r"C:\Users\bpqvg\source\repos\wotlk-utils\wotlk\docs\warden_dumps"

# Known type IDs for modules we've confirmed
KNOWN_TYPES = {
    '7C4ABC97': {
        0x1F: 'TIMING', 0x22: 'PAGE_A', 0x47: 'PAGE_B',
        0x69: 'PROC', 0x8E: 'MEM', 0x91: 'MPQ',
        0xB3: 'MODULE', 0xD8: 'DRIVER', 0xDB: 'LUA'
    },
    'DA3BF29E': {0x74: 'TIMING', 0x70: 'LUA'},
}

# DFS-learned types from runtime
DFS_LEARNED = {
    '037305': {0x91: '0B', 0x16: '0B', 0xAE: '1B', 0xC2: '2B', 0x3B: '25B'},
}

def decompress_module(decrypted_data):
    if len(decrypted_data) < 5:
        return None
    decomp_size = struct.unpack_from('<I', decrypted_data, 0)[0]
    if decomp_size == 0 or decomp_size > 4*1024*1024:
        return None
    if decrypted_data[4] != 0x78:
        return None
    try:
        return zlib.decompress(decrypted_data[4:])
    except zlib.error:
        return None

def load_module(hash_str, dirs):
    for d in dirs:
        path = os.path.join(d, f"warden_{hash_str}_decompressed.bin")
        if os.path.exists(path):
            with open(path, 'rb') as f:
                return f.read()
        path = os.path.join(d, f"warden_{hash_str}_decrypted.bin")
        if os.path.exists(path):
            with open(path, 'rb') as f:
                decrypted = f.read()
            data = decompress_module(decrypted)
            if data:
                decomp_path = os.path.join(d, f"warden_{hash_str}_decompressed.bin")
                with open(decomp_path, 'wb') as f:
                    f.write(data)
                return data
    return None

def parse_header(data):
    if len(data) < 0x28:
        return None
    fields = struct.unpack_from('<10I', data, 0)
    names = ['moduleSize', 'reserved', 'relocDataOff', 'relocCount',
             'exportTableOff', 'exportCount', 'baseIndex',
             'importTableOff', 'importLibCount', 'sectionDescCount']
    h = dict(zip(names, fields))
    h['packedDataOff'] = 0x28 + h['sectionDescCount'] * 12
    return h

def find_all_remap_tables(data):
    """Find ALL remap+jump table patterns by raw byte scan."""
    results = []
    for off in range(len(data) - 7):
        if data[off] != 0x0F or data[off+1] != 0xB6:
            continue
        modrm = data[off+2]
        if not (0x80 <= modrm <= 0xBF) or (modrm & 7) == 4:
            continue
        remap_off = struct.unpack_from('<I', data, off+3)[0]
        if remap_off < 0x100 or remap_off + 256 > len(data):
            continue
        for j in range(off+7, min(off+57, len(data)-7)):
            if data[j] != 0xFF or data[j+1] != 0x24:
                continue
            sib = data[j+2]
            if (sib >> 6) != 2 or (sib & 7) != 5:
                continue
            jtable_off = struct.unpack_from('<I', data, j+3)[0]
            # Look backwards for range check
            max_type = 0xFF
            for k in range(off-1, max(0, off-40), -1):
                if k + 5 <= len(data) and data[k] == 0x3D:
                    val = struct.unpack_from('<I', data, k+1)[0]
                    if 0x80 <= val <= 0xFF:
                        max_type = val
                        break
                if k + 2 <= len(data) and data[k] == 0x3C:
                    val = data[k+1]
                    if 0x80 <= val <= 0xFF:
                        max_type = val
                        break
            results.append({
                'movzx_off': off,
                'jmp_off': j,
                'remap_off': remap_off,
                'jtable_off': jtable_off,
                'max_type': max_type,
            })
    return results

def extract_groups(data, remap_off, jtable_off, max_type):
    """Extract handler groups from remap table."""
    table = data[remap_off:remap_off + max_type + 1]

    # Count occurrences per index
    counts = {}
    for i in range(len(table)):
        idx = table[i]
        counts[idx] = counts.get(idx, 0) + 1

    # Default = most frequent
    default_idx = max(counts.keys(), key=lambda k: counts[k])

    # Group by handler index
    groups = {}
    for i in range(len(table)):
        idx = table[i]
        if idx != default_idx:
            if idx not in groups:
                groups[idx] = []
            groups[idx].append(i)

    # Read jump table entries to find handler addresses
    max_idx = max(groups.keys()) + 1 if groups else 0
    handler_addrs = {}
    for idx in groups:
        off = jtable_off + idx * 4
        if off + 4 <= len(data):
            handler_addrs[idx] = struct.unpack_from('<I', data, off)[0]

    # Find groups that share the same handler address
    addr_to_groups = {}
    for idx, addr in handler_addrs.items():
        if addr not in addr_to_groups:
            addr_to_groups[addr] = []
        addr_to_groups[addr].append(idx)

    return table, groups, default_idx, handler_addrs, addr_to_groups

def find_xor_before_remap(data, movzx_off):
    """Find the xor instruction before the remap lookup by raw byte scan."""
    # Looking for: 32 XX 04 (xor r8, byte ptr [reg + 4])
    # or: 30 XX 04 (xor byte ptr [reg + 4], r8)
    for off in range(movzx_off - 1, max(0, movzx_off - 60), -1):
        if data[off] == 0x32:  # xor r8, r/m8
            modrm = data[off+1]
            if 0x40 <= modrm <= 0x7F and (modrm & 7) != 4:  # [reg + disp8]
                disp = data[off+2]
                if disp == 4:  # [reg + 4]
                    return off, 'xor_r8_mem8'
    return None, None

def analyze_module(hash_str, data, short_hash):
    header = parse_header(data)
    if not header:
        print(f"  ERROR: Invalid header")
        return

    print(f"  Size: {len(data)} bytes")

    remap_results = find_all_remap_tables(data)
    print(f"  Remap+JMP patterns: {len(remap_results)}")

    all_type_ids = set()

    for i, r in enumerate(remap_results):
        print(f"\n  --- Remap table #{i+1} ---")
        print(f"    movzx @ 0x{r['movzx_off']:04x}, jmp @ 0x{r['jmp_off']:04x}")
        print(f"    remap=0x{r['remap_off']:04x}, jtable=0x{r['jtable_off']:04x}, max=0x{r['max_type']:02x}")

        # Find XOR
        xor_off, xor_type = find_xor_before_remap(data, r['movzx_off'])
        if xor_off is not None:
            raw = ' '.join(f'{data[xor_off+k]:02x}' for k in range(3))
            print(f"    XOR found at 0x{xor_off:04x}: {raw}")
        else:
            print(f"    XOR: not found in 60 bytes before movzx")

        table, groups, default_idx, handler_addrs, addr_to_groups = \
            extract_groups(data, r['remap_off'], r['jtable_off'], r['max_type'])

        print(f"    Default handler: index {default_idx} ({sum(1 for v in table[:r['max_type']+1] if v == default_idx)} entries)")
        print(f"    Non-default groups: {len(groups)}")

        # Show groups by size
        groups_sorted = sorted(groups.items(), key=lambda x: len(x[1]))

        # Find groups with shared handler addresses (same code)
        unique_handlers = len(addr_to_groups)
        print(f"    Unique handler addresses: {unique_handlers}")

        # Groups with same handler address
        merged_groups = {}
        for addr, indices in addr_to_groups.items():
            all_types_for_addr = []
            for idx in indices:
                all_types_for_addr.extend(groups[idx])
            merged_groups[addr] = sorted(all_types_for_addr)

        print(f"    Merged groups (by handler address): {len(merged_groups)}")

        # Show each merged group
        for addr in sorted(merged_groups.keys()):
            types = merged_groups[addr]
            all_type_ids.update(types)
            indices = addr_to_groups[addr]
            known_str = ""
            known_info = KNOWN_TYPES.get(short_hash, {})
            for t in types:
                if t in known_info:
                    known_str += f" [0x{t:02x}={known_info[t]}]"
            dfs_info = DFS_LEARNED.get(short_hash[:6], {})
            for t in types:
                if t in dfs_info:
                    known_str += f" [0x{t:02x}=DFS:{dfs_info[t]}]"
            if len(types) <= 20:
                print(f"      handler 0x{addr:08x} ({len(types):2d} types, idx={indices}): "
                      f"{[f'0x{t:02x}' for t in types]}{known_str}")
            else:
                print(f"      handler 0x{addr:08x} ({len(types):2d} types, idx={indices}): "
                      f"[0x{types[0]:02x}..0x{types[-1]:02x}]{known_str}")

    if all_type_ids:
        print(f"\n  ALL valid type IDs (union of all remap tables): "
              f"{sorted([f'0x{t:02x}' for t in all_type_ids])}")
        print(f"  Count: {len(all_type_ids)}")

        # Cross-validate with known types
        known = KNOWN_TYPES.get(short_hash, {})
        if known:
            found = set(known.keys()) & all_type_ids
            missing = set(known.keys()) - all_type_ids
            print(f"\n  VALIDATION vs known types:")
            print(f"    Found: {sorted([f'0x{t:02x}={known[t]}' for t in found])}")
            if missing:
                print(f"    MISSING: {sorted([f'0x{t:02x}={known[t]}' for t in missing])}")
            else:
                print(f"    All known types found!")

def main():
    dirs = [DUMP_DIR, DOCS_DIR]
    hashes = set()
    for d in dirs:
        if not os.path.exists(d):
            continue
        for f in os.listdir(d):
            if f.startswith('warden_') and ('_decompressed' in f or '_decrypted' in f):
                parts = f.split('_')
                if len(parts) >= 3 and len(parts[1]) == 32:
                    hashes.add(parts[1])

    print(f"Found {len(hashes)} unique modules\n")

    for h in sorted(hashes):
        short = h[:8]
        print(f"\n{'='*70}")
        print(f"Module: {short} ({h})")
        print(f"{'='*70}")

        data = load_module(h, dirs)
        if data is None:
            print(f"  SKIP: load failed")
            continue

        analyze_module(h, data, short)

if __name__ == '__main__':
    main()
