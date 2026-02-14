"""
Analyze remap table GROUP SIZES across all modules.

Hypothesis: Real check types are in the SMALLEST groups (singletons or pairs).
Obfuscation creates many type bytes sharing the same handler index.
Real types each get their own unique handler → singleton groups.

For 7C4ABC97 (known types: 0x1F,0x22,0x47,0x69,0x8E,0x91,0xB3,0xD8,0xDB):
- 6 singletons: 0x1F,0x69,0x8E,0x91,0xD8,0xDB
- 1 pair: [0x22,0x47] (PAGE_A/PAGE_B share handler)
- 0xB3: maps to DEFAULT (missing)

If this holds, singletons + small groups = real type IDs.
"""

import struct
import zlib
import os

DUMP_DIR = r"Z:\Games\wow 3.3.5a client\warden_dumps"
DOCS_DIR = r"C:\Users\bpqvg\source\repos\wotlk-utils\wotlk\docs\warden_dumps"

KNOWN_TYPES = {
    '7C4ABC97': {
        0x1F: 'TIMING', 0x22: 'PAGE_A', 0x47: 'PAGE_B',
        0x69: 'PROC', 0x8E: 'MEM', 0x91: 'MPQ',
        0xB3: 'MODULE', 0xD8: 'DRIVER', 0xDB: 'LUA'
    },
}

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
                return decompress_module(f.read())
    return None

def find_remap_tables(data):
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
            results.append((remap_off, jtable_off, max_type))
    return results

def analyze_groups(data, remap_off, jtable_off, max_type, short_hash, table_idx):
    table = data[remap_off:remap_off + max_type + 1]

    # Count per index
    counts = {}
    for i in range(len(table)):
        idx = table[i]
        counts[idx] = counts.get(idx, 0) + 1

    default_idx = max(counts.keys(), key=lambda k: counts[k])

    # Group by handler index
    groups = {}
    for i in range(len(table)):
        idx = table[i]
        if idx != default_idx:
            if idx not in groups:
                groups[idx] = []
            groups[idx].append(i)

    # Sort groups by size
    sorted_groups = sorted(groups.items(), key=lambda x: len(x[1]))

    # Group size distribution
    size_dist = {}
    for idx, members in groups.items():
        sz = len(members)
        if sz not in size_dist:
            size_dist[sz] = 0
        size_dist[sz] += 1

    known = KNOWN_TYPES.get(short_hash, {})
    dfs = DFS_LEARNED.get(short_hash[:6], {})

    print(f"\n  Table #{table_idx}: remap=0x{remap_off:04x}, max=0x{max_type:02x}, "
          f"default_idx={default_idx}, {len(groups)} groups")
    print(f"    Group size distribution: ", end="")
    for sz in sorted(size_dist.keys()):
        print(f"size={sz}:{size_dist[sz]}  ", end="")
    print()

    # Show groups ordered by size (smallest first)
    print(f"\n    Smallest groups (candidates for real type IDs):")
    for idx, members in sorted_groups:
        if len(members) > 5:  # Skip large groups
            remaining = len(sorted_groups) - sorted_groups.index((idx, members))
            print(f"    ... {remaining} more groups with size > 5 ...")
            break
        known_str = ""
        for t in members:
            if t in known:
                known_str += f" [{known[t]}]"
            if t in dfs:
                known_str += f" [DFS:{dfs[t]}]"
        print(f"      idx={idx:3d}, size={len(members)}: "
              f"{[f'0x{t:02x}' for t in sorted(members)]}{known_str}")

    # Summary: singletons + pairs
    singletons = [m[0] for idx, m in sorted_groups if len(m) == 1]
    pairs = [sorted(m) for idx, m in sorted_groups if len(m) == 2]
    triples = [sorted(m) for idx, m in sorted_groups if len(m) == 3]

    print(f"\n    Singletons ({len(singletons)}): {[f'0x{t:02x}' for t in sorted(singletons)]}")
    print(f"    Pairs ({len(pairs)}): {[f'[0x{a:02x},0x{b:02x}]' for a,b in sorted(pairs)]}")
    if triples:
        print(f"    Triples ({len(triples)}): {[[f'0x{t:02x}' for t in g] for g in sorted(triples)]}")

    # Validate against known types
    if known:
        all_small = set(singletons)
        for p in pairs:
            all_small.update(p)
        found = set(known.keys()) & all_small
        missing = set(known.keys()) - all_small
        in_default = set()
        for t in missing:
            if t <= max_type and table[t] == default_idx:
                in_default.add(t)
        print(f"\n    VALIDATION: known types in singletons+pairs: "
              f"{[f'0x{t:02x}={known[t]}' for t in sorted(found)]}")
        if missing:
            print(f"    MISSING from small groups: "
                  f"{[f'0x{t:02x}={known[t]}' for t in sorted(missing)]}")
        if in_default:
            print(f"    In DEFAULT: "
                  f"{[f'0x{t:02x}={known[t]}' for t in sorted(in_default)]}")

    return singletons, pairs

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

    print(f"Analyzing group sizes across {len(hashes)} modules\n")

    for h in sorted(hashes):
        short = h[:8]
        print(f"\n{'='*70}")
        print(f"Module: {short}")
        print(f"{'='*70}")

        data = load_module(h, dirs)
        if data is None:
            print("  SKIP")
            continue

        remap_tables = find_remap_tables(data)
        print(f"  {len(remap_tables)} remap table(s)")

        for i, (remap_off, jtable_off, max_type) in enumerate(remap_tables):
            analyze_groups(data, remap_off, jtable_off, max_type, short, i+1)

if __name__ == '__main__':
    main()
