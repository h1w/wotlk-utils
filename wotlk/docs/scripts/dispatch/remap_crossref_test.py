"""
Test improved remap cross-referencing: pair-based grouping.

For each byte value 0-255, create a tuple (table1[byte], table2[byte]).
Group byte values by their tuples. Singletons = check types.
"""

import struct
import zlib
import os
from capstone import *
from capstone.x86 import *

DUMP_DIRS = [
    r"Z:\Games\wow 3.3.5a client\warden_dumps",
    r"C:\Users\bpqvg\source\repos\wotlk-utils\wotlk\docs\warden_dumps",
]

# Known reference module
KNOWN_TYPES_7C4ABC97 = {
    0x1F: 'TIMING', 0x22: 'PAGE_A', 0x47: 'PAGE_B',
    0x69: 'PROC', 0x8E: 'MEM', 0x91: 'MPQ',
    0xB3: 'MODULE', 0xD8: 'DRIVER', 0xDB: 'LUA'
}

def load_module(hash_str):
    for d in DUMP_DIRS:
        if not os.path.exists(d):
            continue
        path = os.path.join(d, f"warden_{hash_str}_decompressed.bin")
        if os.path.exists(path):
            with open(path, 'rb') as f:
                return f.read()
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

MAX_MOVZX_DIST = 18

def find_xor_sites(data, scan_start):
    sites = []
    for i in range(scan_start, len(data) - 2):
        if data[i] != 0x32:
            continue
        modrm = data[i + 1]
        if not (0x40 <= modrm <= 0x7F):
            continue
        if (modrm & 7) == 4:
            continue
        if data[i + 2] != 0x04:
            continue
        for j in range(i + 3, min(i + 3 + MAX_MOVZX_DIST, len(data) - 2)):
            if data[j] == 0x0F and data[j+1] == 0xB6 and data[j+2] >= 0xC0:
                sites.append({'xor_off': i, 'movzx_off': j, 'after_movzx': j + 3})
                break
    return sites

def find_remap_table(data, size, after_movzx):
    """Find remap table: movzx byte [reg+large_disp] pattern."""
    md = Cs(CS_ARCH_X86, CS_MODE_32)
    md.detail = True
    code = data[after_movzx:min(after_movzx + 80, size)]
    insns = list(md.disasm(code, after_movzx))

    for i in range(min(20, len(insns))):
        insn = insns[i]
        if insn.mnemonic == 'movzx' and len(insn.operands) == 2:
            src = insn.operands[1]
            if src.type == CS_OP_MEM and src.size == 1 and src.mem.disp > 0x100:
                remap_off = src.mem.disp
                for j in range(i+1, min(i+8, len(insns))):
                    if insns[j].mnemonic == 'jmp' and len(insns[j].operands) == 1:
                        op = insns[j].operands[0]
                        if op.type == CS_OP_MEM and op.mem.scale == 4:
                            return remap_off, op.mem.disp
    return None, None

def analyze_remap_module(hash_str, data):
    header = parse_header(data)
    if not header:
        return
    scan_start = header['packedDataOff']
    sites = find_xor_sites(data, scan_start)

    remap_tables = []
    for site in sites:
        remap_off, jtable_off = find_remap_table(data, len(data), site['after_movzx'])
        if remap_off is not None:
            remap_tables.append({
                'xor_off': site['xor_off'],
                'remap_off': remap_off,
                'jtable_off': jtable_off
            })

    if len(remap_tables) < 2:
        print(f"  {hash_str[:8]}: Only {len(remap_tables)} remap table(s), need 2 for cross-ref")
        return

    print(f"\n{'='*70}")
    print(f"Module: {hash_str[:8]} — {len(remap_tables)} remap tables")
    print(f"{'='*70}")

    for i, rt in enumerate(remap_tables):
        print(f"  Table {i}: XOR@0x{rt['xor_off']:04x}, remap@0x{rt['remap_off']:04x}, "
              f"jtable@0x{rt['jtable_off']:04x}")

    # Read both remap tables (256 entries each)
    tables = []
    for rt in remap_tables:
        off = rt['remap_off']
        if off + 256 > len(data):
            print(f"  Table at 0x{off:04x} out of bounds!")
            return
        tables.append(data[off:off+256])

    # Method 1: Per-table singleton analysis (current approach)
    print(f"\n  --- Method 1: Per-table singletons (current C++ approach) ---")
    for i, table in enumerate(tables):
        counts = {}
        for b in table:
            counts[b] = counts.get(b, 0) + 1
        default_idx = max(counts.keys(), key=lambda k: counts[k])
        groups = {}
        for j in range(256):
            idx = table[j]
            if idx != default_idx:
                groups.setdefault(idx, []).append(j)
        singletons = [g[0] for g in groups.values() if len(g) == 1]
        pairs = [g for g in groups.values() if len(g) == 2]
        print(f"  Table {i}: default=0x{default_idx:02X} ({counts[default_idx]} entries), "
              f"{len(groups)} non-default groups, "
              f"{len(singletons)} singletons, {len(pairs)} pairs")
        if singletons:
            print(f"    Singletons: {' '.join(f'0x{s:02X}' for s in sorted(singletons))}")

    # Method 2: Pair-based cross-referencing (new approach)
    print(f"\n  --- Method 2: Pair-based cross-referencing (NEW) ---")
    t1 = tables[0]
    t2 = tables[1]

    # Group byte values by (table1[byte], table2[byte]) pair
    pair_groups = {}
    for byte_val in range(256):
        pair_key = (t1[byte_val], t2[byte_val])
        pair_groups.setdefault(pair_key, []).append(byte_val)

    # Find singletons (pair groups with exactly 1 byte value)
    singleton_types = []
    small_groups = []
    for pair_key, members in sorted(pair_groups.items(), key=lambda x: len(x[1])):
        if len(members) == 1:
            singleton_types.append(members[0])
        elif len(members) <= 3:
            small_groups.append((pair_key, members))

    print(f"  Total pair groups: {len(pair_groups)}")
    print(f"  Singleton groups (1 member): {len(singleton_types)}")
    if singleton_types:
        print(f"  Singleton types: {' '.join(f'0x{t:02X}' for t in sorted(singleton_types))}")
    if small_groups:
        print(f"  Small groups (2-3 members): {len(small_groups)}")
        for pair_key, members in small_groups:
            print(f"    handlers=({pair_key[0]:02X},{pair_key[1]:02X}): "
                  f"{' '.join(f'0x{m:02X}' for m in members)}")

    # Show largest groups for context
    largest = sorted(pair_groups.items(), key=lambda x: -len(x[1]))[:5]
    print(f"  Largest groups:")
    for pair_key, members in largest:
        print(f"    handlers=({pair_key[0]:02X},{pair_key[1]:02X}): {len(members)} members")

    return singleton_types

# Modules to test (including reference module + remap-only modules)
TEST_MODULES = [
    "7C4ABC9770E0D917D752C0F873CF7525",  # reference (known types)
    "0AC0C55913C8F3C5EB6E32CCA64A6AA6",  # remap-only
    "CB9E43D692620E7B698C5CE085163E6E",  # remap-only
    "E348326F76A7B5E7C2A8F06D67C65BED",  # remap-only
    "0BE6B21C29A30A89C2AF0AFF8F23EB0D",  # partial (5 types from chain)
]

def main():
    hashes = set()
    for d in DUMP_DIRS:
        if not os.path.exists(d):
            continue
        for f in os.listdir(d):
            if f.startswith('warden_') and '_decompressed' in f:
                parts = f.split('_')
                if len(parts) >= 3 and len(parts[1]) == 32:
                    hashes.add(parts[1])

    results = {}
    for h in sorted(hashes):
        data = load_module(h)
        if data is None:
            continue
        singletons = analyze_remap_module(h, data)
        if singletons:
            results[h[:8]] = singletons

    # Validate against known types for 7C4ABC97
    if '7C4ABC97' in results:
        known = set(KNOWN_TYPES_7C4ABC97.keys())
        found = set(results['7C4ABC97'])
        if known <= found:
            print(f"\n7C4ABC97 VALIDATION: All {len(known)} known types found in pair singletons!")
        else:
            missing = known - found
            extra = found - known
            print(f"\n7C4ABC97 VALIDATION:")
            if missing:
                print(f"  MISSING: {' '.join(f'0x{t:02X}={KNOWN_TYPES_7C4ABC97[t]}' for t in sorted(missing))}")
            if extra:
                print(f"  EXTRA: {' '.join(f'0x{t:02X}' for t in sorted(extra))}")

if __name__ == '__main__':
    main()
