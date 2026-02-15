"""Debug remap tables for 0AC0C559 and E348326F + reference module 7C4ABC97.
Group by INDEX VALUE (raw byte in remap table), NOT handler address.
"""

import struct, os
from capstone import *
from capstone.x86 import *

DOCS_DIR = r"C:\Users\bpqvg\source\repos\wotlk-utils\wotlk\docs\warden_dumps"

KNOWN_TYPES = {
    '7C4ABC97': {
        0x1F: 'TIMING', 0x22: 'PAGE_A', 0x47: 'PAGE_B',
        0x69: 'PROC', 0x8E: 'MEM', 0x91: 'MPQ',
        0xB3: 'MODULE', 0xD8: 'DRIVER', 0xDB: 'LUA'
    },
}

md = Cs(CS_ARCH_X86, CS_MODE_32)
md.detail = True


def load_module(hash_str):
    path = os.path.join(DOCS_DIR, f"warden_{hash_str}_decompressed.bin")
    if os.path.exists(path):
        with open(path, 'rb') as f:
            return f.read()
    return None


def raw_scan_xor(data, disp_val=4):
    results = []
    for off in range(len(data) - 2):
        if data[off] != 0x32:
            continue
        modrm = data[off + 1]
        if not (0x40 <= modrm <= 0x7F):
            continue
        if (modrm & 7) == 4:
            continue
        if data[off + 2] != disp_val:
            continue
        results.append(off)
    return results


def find_remap_info(data, xor_off):
    """Find remap table offset and shift from XOR site."""
    end = min(xor_off + 120 * 15, len(data))
    insns = list(md.disasm(data[xor_off:end], xor_off))

    # Find movzx r32, r8
    movzx_idx = None
    for i, insn in enumerate(insns):
        if insn.mnemonic == 'movzx' and len(insn.operands) == 2:
            dst, src = insn.operands
            if dst.type == CS_OP_REG and (src.type == CS_OP_REG and src.size == 1):
                movzx_idx = i
                break
    if movzx_idx is None:
        return None

    window = insns[movzx_idx + 1:movzx_idx + 25]

    # Detect shift and max_type
    shift = 0
    max_type = 0xFF
    for insn in window:
        if insn.mnemonic == 'add' and len(insn.operands) == 2:
            if insn.operands[1].type == CS_OP_IMM:
                val = insn.operands[1].imm
                # Python capstone returns signed: add eax, -113 → val = -113
                if val < 0:
                    shift = (-val) & 0xFF
                elif val > 0x80000000:
                    shift = (0x100000000 - val) & 0xFF
        if insn.mnemonic == 'sub' and len(insn.operands) == 2:
            if insn.operands[0].type == CS_OP_REG and insn.operands[1].type == CS_OP_IMM:
                val = insn.operands[1].imm & 0xFF
                if 0 < val < 0x80:
                    shift = val
        if insn.mnemonic == 'cmp' and len(insn.operands) == 2:
            if insn.operands[1].type == CS_OP_IMM:
                val = insn.operands[1].imm & 0xFFFFFFFF
                if val < 0:
                    val = val & 0xFFFFFFFF
                if 0x80 <= (val & 0xFF) <= 0xFF and val <= 0xFF:
                    max_type = val

    # Find movzx byte ptr [reg + large_disp] (remap lookup)
    for i, insn in enumerate(window):
        if insn.mnemonic == 'movzx' and len(insn.operands) == 2:
            src = insn.operands[1]
            if src.type == CS_OP_MEM and src.size == 1:
                disp = src.mem.disp
                if disp < 0:
                    disp = disp & 0xFFFFFFFF
                if disp > 0x100:
                    # Look for jmp [reg*4 + disp] nearby
                    for j in range(i + 1, min(i + 8, len(window))):
                        w = window[j]
                        if w.mnemonic == 'jmp' and len(w.operands) == 1:
                            op = w.operands[0]
                            if op.type == CS_OP_MEM and op.mem.scale == 4:
                                jt_disp = op.mem.disp
                                if jt_disp < 0:
                                    jt_disp = jt_disp & 0xFFFFFFFF
                                return {
                                    'remap_off': disp,
                                    'jtable_off': jt_disp,
                                    'shift': shift,
                                    'max_type': max_type,
                                    'xor_off': xor_off,
                                }
    return None


def analyze_remap_by_index(data, info, label, known=None):
    """Analyze remap table grouping by INDEX VALUE."""
    remap_off = info['remap_off']
    shift = info['shift']
    max_type = info['max_type']

    if remap_off + max_type + 1 > len(data):
        print(f"  {label}: remap table extends beyond data")
        return None

    table = data[remap_off:remap_off + max_type + 1]

    # Count occurrences of each index value
    index_counts = {}
    for idx in table:
        index_counts[idx] = index_counts.get(idx, 0) + 1

    # Default = most common index value
    default_idx = max(index_counts.keys(), key=lambda k: index_counts[k])
    default_count = index_counts[default_idx]

    # Group type bytes by index value
    groups = {}  # index_value -> list of type bytes
    for i in range(len(table)):
        idx = table[i]
        if idx != default_idx:
            if idx not in groups:
                groups[idx] = []
            groups[idx].append(i)

    # Sort by group size
    sorted_groups = sorted(groups.items(), key=lambda x: len(x[1]))

    # Size distribution
    size_dist = {}
    for idx, members in groups.items():
        sz = len(members)
        size_dist[sz] = size_dist.get(sz, 0) + 1

    print(f"\n  {label}: remap=0x{remap_off:04x}, shift=0x{shift:02x}, max=0x{max_type:02x}")
    print(f"    default_idx={default_idx}, {default_count}/{max_type+1} entries are default")
    print(f"    {len(groups)} non-default groups")
    print(f"    Size distribution: ", end="")
    for sz in sorted(size_dist.keys()):
        print(f"sz{sz}={size_dist[sz]} ", end="")
    print()

    # Show all groups
    singletons = []
    pairs = []
    triples = []
    for idx, members in sorted_groups:
        real_members = [(m + shift) & 0xFF for m in members]
        ann = ""
        if known:
            for t in real_members:
                if t in known:
                    ann += f" [{known[t]}]"
        if len(members) == 1:
            singletons.extend(real_members)
        elif len(members) == 2:
            pairs.append(real_members)
        elif len(members) == 3:
            triples.append(real_members)

        if len(members) <= 5:
            print(f"      idx={idx:3d} (sz={len(members)}): "
                  f"raw={[f'0x{m:02x}' for m in sorted(members)]} "
                  f"real={[f'0x{t:02x}' for t in sorted(real_members)]}{ann}")
        else:
            print(f"      idx={idx:3d} (sz={len(members)}): {len(members)} entries (noise)")

    print(f"\n    Singletons ({len(singletons)}): {[f'0x{t:02x}' for t in sorted(singletons)]}")
    print(f"    Pairs ({len(pairs)}): {[f'[{a:#04x},{b:#04x}]' for a, b in sorted(pairs)]}")
    if triples:
        print(f"    Triples ({len(triples)}): {triples}")

    # Return non-default entries grouped by index
    return {
        'default_idx': default_idx,
        'groups': groups,
        'singletons': set(singletons),
        'pairs': pairs,
        'shift': shift,
    }


def investigate_module(hash_str, label, known=None):
    print(f"\n{'='*70}")
    print(f"Module: {label} ({hash_str[:8]})")
    print(f"{'='*70}")

    data = load_module(hash_str)
    if not data:
        print("  NOT FOUND")
        return

    print(f"  Size: {len(data)} bytes")

    xor_offs = raw_scan_xor(data)
    print(f"  XOR+[reg+4] sites: {len(xor_offs)}")

    remap_results = []
    for i, off in enumerate(xor_offs):
        info = find_remap_info(data, off)
        if info:
            result = analyze_remap_by_index(data, info, f"Table {i+1} (XOR@0x{off:04x})", known)
            if result:
                remap_results.append((info, result))
        else:
            print(f"\n  Table {i+1} (XOR@0x{off:04x}): NOT a remap table")

    # Cross-reference: find types that are non-default in BOTH tables
    if len(remap_results) >= 2:
        print(f"\n  === CROSS-REFERENCE ===")
        all_non_default_sets = []
        for info, result in remap_results:
            remap_off = info['remap_off']
            shift = result['shift']
            # Get all non-default type bytes (with shift applied)
            non_default = set()
            for idx, members in result['groups'].items():
                for m in members:
                    non_default.add((m + shift) & 0xFF)
            all_non_default_sets.append(non_default)
            print(f"    Table remap=0x{remap_off:04x}: {len(non_default)} non-default types")

        intersection = all_non_default_sets[0]
        for s in all_non_default_sets[1:]:
            intersection = intersection & s

        print(f"    Intersection (non-default in ALL tables): {len(intersection)} types")
        print(f"    Types: {[f'0x{t:02x}' for t in sorted(intersection)]}")

        # Also try: singletons+pairs intersection
        sp_sets = []
        for info, result in remap_results:
            sp = set(result['singletons'])
            for pair in result['pairs']:
                sp.update(pair)
            sp_sets.append(sp)

        sp_intersection = sp_sets[0]
        for s in sp_sets[1:]:
            sp_intersection = sp_intersection & s
        print(f"    Singletons+pairs intersection: {len(sp_intersection)} types")
        print(f"    Types: {[f'0x{t:02x}' for t in sorted(sp_intersection)]}")

        if known:
            found = set(known.keys()) & intersection
            missing = set(known.keys()) - intersection
            print(f"\n    VALIDATION (all-non-default intersection):")
            print(f"      Found: {[f'0x{t:02x}={known[t]}' for t in sorted(found)]}")
            if missing:
                print(f"      Missing: {[f'0x{t:02x}={known[t]}' for t in sorted(missing)]}")


if __name__ == '__main__':
    # Reference module first
    investigate_module("7C4ABC97B86494A2D91820785F3A1C87", "7C4ABC97 (reference)", KNOWN_TYPES.get('7C4ABC97'))
    # Failing modules
    investigate_module("0AC0C5599EB0FA9FC3392907F3A2918C", "0AC0C559")
    investigate_module("E348326FA04D9CD237FCDA4B473D97B0", "E348326F")
