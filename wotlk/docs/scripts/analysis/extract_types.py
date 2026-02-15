"""
Extract check type IDs from remap tables in Warden modules.
Now that we know the exact pattern, extract the type mappings.
"""

import struct
from capstone import *

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

def disasm_around(data, offset, window=80):
    md = Cs(CS_ARCH_X86, CS_MODE_32)
    md.detail = True
    start = max(0, offset - window)
    end = min(len(data), offset + window)
    code = data[start:end]
    for insn in md.disasm(code, start):
        marker = " <<<<" if insn.address == offset else ""
        raw = ' '.join(f'{b:02x}' for b in insn.bytes)
        print(f"  0x{insn.address:04x}: {raw:24s} {insn.mnemonic:8s} {insn.op_str}{marker}")

def find_all_remap_jump_patterns(data):
    """
    Find ALL instances of the pattern:
      movzx r32, byte ptr [r32 + REMAP_OFFSET]  ; remap table lookup
      ... (0-10 instructions gap) ...
      jmp dword ptr [r32*4 + JTABLE_OFFSET]      ; jump table dispatch

    Also look for the preceding range check:
      cmp reg, MAX_VALUE
      ja  default_handler
    """
    results = []

    # Find all movzx r32, byte [r32 + disp32] where disp > 0x100
    movzx_locs = []
    for off in range(len(data) - 7):
        if data[off] == 0x0F and data[off+1] == 0xB6:
            modrm = data[off+2]
            if 0x80 <= modrm <= 0xBF and (modrm & 7) != 4:  # not SIB
                disp = struct.unpack_from('<I', data, off+3)[0]
                if 0x100 < disp < len(data):
                    reg_dst = (modrm >> 3) & 7
                    reg_src = modrm & 7
                    movzx_locs.append((off, reg_dst, reg_src, disp))

    # Find all jmp [r32*4 + disp32]
    jmp_locs = []
    for off in range(len(data) - 7):
        if data[off] == 0xFF and data[off+1] == 0x24:
            sib = data[off+2]
            scale = sib >> 6
            index = (sib >> 3) & 7
            base_reg = sib & 7
            if scale == 2 and base_reg == 5:  # scale=4 (2^2), base=disp32 (5=none)
                disp = struct.unpack_from('<I', data, off+3)[0]
                jmp_locs.append((off, index, disp))

    # Match movzx with nearby jmp
    for m_off, m_dst, m_src, m_disp in movzx_locs:
        for j_off, j_idx, j_disp in jmp_locs:
            if 0 < j_off - m_off < 50:  # jmp should be within 50 bytes after movzx
                results.append({
                    'movzx_offset': m_off,
                    'jmp_offset': j_off,
                    'remap_table_offset': m_disp,
                    'jump_table_offset': j_disp,
                    'movzx_dst_reg': m_dst,
                    'jmp_index_reg': j_idx,
                })

    return results

def extract_type_ids(data, remap_offset, max_type=0xFF):
    """
    Read the remap table. Entries that map to unique handler indices
    are the check type IDs. The most common index is the "default/error" handler.
    """
    if remap_offset + 256 > len(data):
        return None

    table = data[remap_offset:remap_offset + 256]

    # Count how many type values map to each index
    index_counts = {}
    for i in range(min(256, max_type + 1)):
        idx = table[i]
        if idx not in index_counts:
            index_counts[idx] = []
        index_counts[idx].append(i)

    # The default handler index = the most common one
    default_idx = max(index_counts.keys(), key=lambda k: len(index_counts[k]))

    # Active types = those NOT mapping to default
    active_types = {}
    for idx, type_vals in sorted(index_counts.items()):
        if idx != default_idx:
            active_types[idx] = type_vals

    return table, active_types, default_idx, index_counts

def find_range_check(data, movzx_offset):
    """Look backwards from movzx for a cmp + ja pattern to find max type value."""
    md = Cs(CS_ARCH_X86, CS_MODE_32)
    md.detail = True

    search_start = max(0, movzx_offset - 40)
    code = data[search_start:movzx_offset + 7]

    max_type = 0xFF
    for insn in md.disasm(code, search_start):
        if insn.address >= movzx_offset:
            break
        if insn.mnemonic in ('cmp', 'test') and insn.op_count(CS_OP_IMM) > 0:
            for op in insn.operands:
                if op.type == CS_OP_IMM:
                    v = op.imm & 0xFF
                    if 0x80 <= v <= 0xFF and op.imm == v:
                        max_type = v
    return max_type

def analyze_module(path, label, known_types=None):
    print(f"\n{'='*70}")
    print(f"Module: {label}")
    print(f"{'='*70}")

    data = read_module(path)
    header = parse_header(data)

    print(f"Size: {len(data)} bytes, runtime: {header['moduleSize']}")
    print(f"Packed data offset: 0x{header['packedDataOff']:04x}")

    patterns = find_all_remap_jump_patterns(data)
    print(f"\nFound {len(patterns)} remap+jump table pattern(s)")

    for i, p in enumerate(patterns):
        print(f"\n--- Pattern #{i+1} ---")
        print(f"  movzx at file offset 0x{p['movzx_offset']:04x}")
        print(f"  jmp   at file offset 0x{p['jmp_offset']:04x}")
        print(f"  Remap table offset:  0x{p['remap_table_offset']:04x}")
        print(f"  Jump table offset:   0x{p['jump_table_offset']:04x}")

        regs = ['eax','ecx','edx','ebx','esp','ebp','esi','edi']
        print(f"  movzx dst reg: {regs[p['movzx_dst_reg']]}")
        print(f"  jmp index reg: {regs[p['jmp_index_reg']]}")

        # Disassemble around the pattern
        print(f"\n  Disassembly:")
        disasm_around(data, p['movzx_offset'], window=60)

        # Find range check
        max_type = find_range_check(data, p['movzx_offset'])
        print(f"\n  Max type value (from range check): 0x{max_type:02x}")

        # Extract remap table
        result = extract_type_ids(data, p['remap_table_offset'], max_type)
        if result:
            table, active_types, default_idx, all_counts = result
            print(f"  Default handler index: {default_idx}")
            print(f"  Number of active handler groups: {len(active_types)}")

            # Show handler groups
            # For each unique non-default index, show type values
            print(f"\n  Handler groups (index -> type values):")
            for idx in sorted(active_types.keys()):
                type_vals = active_types[idx]
                # Filter to only types <= max_type
                type_vals = [t for t in type_vals if t <= max_type]
                if type_vals:
                    known_markers = ""
                    if known_types:
                        for t in type_vals:
                            if t in known_types:
                                known_markers += f" [{t:#04x}=KNOWN]"
                    print(f"    index {idx:3d}: {[f'0x{t:02x}' for t in type_vals]}{known_markers}")

            # Extract the TYPE IDs: for groups with exactly 1 member, it's a unique type
            # For groups with many members, they share a handler (like many invalid types going to default)
            # The real check type IDs likely form groups of 1 member per unique handler
            print(f"\n  Likely check type IDs (unique types per handler group):")
            singleton_types = []
            for idx in sorted(active_types.keys()):
                type_vals = [t for t in active_types[idx] if t <= max_type]
                if len(type_vals) == 1:
                    singleton_types.append(type_vals[0])

            # But also consider: the dispatcher has exactly 9 check types
            # Some handler groups may have multiple type values that all map to the same check
            # Show the smallest groups first
            groups_by_size = sorted(active_types.items(), key=lambda x: len([t for t in x[1] if t <= max_type]))
            print(f"\n  All non-default groups sorted by size:")
            for idx, type_vals in groups_by_size:
                type_vals = [t for t in type_vals if t <= max_type]
                if type_vals:
                    print(f"    [{len(type_vals):2d} types] index {idx:3d}: {[f'0x{t:02x}' for t in sorted(type_vals)]}")

            # Read jump table entries
            jtable_off = p['jump_table_offset']
            max_idx = max(active_types.keys()) + 1
            print(f"\n  Jump table entries (at 0x{jtable_off:04x}, {min(max_idx+1, 20)} shown):")
            for j in range(min(max_idx + 1, 30)):
                off = jtable_off + j * 4
                if off + 4 <= len(data):
                    addr = struct.unpack_from('<I', data, off)[0]
                    # Find which types map to this index
                    types_here = all_counts.get(j, [])
                    types_here = [t for t in types_here if t <= max_type and j != default_idx]
                    types_str = f" <- {[f'0x{t:02x}' for t in types_here]}" if types_here else ""
                    default_str = " (DEFAULT)" if j == default_idx else ""
                    print(f"      [{j:2d}] -> 0x{addr:08x}{types_str}{default_str}")

if __name__ == '__main__':
    base = r"C:\Users\bpqvg\source\repos\wotlk-utils\wotlk\docs\warden_dumps"

    # Module 7C4ABC97 - verify with known types
    known_7c = {0x1F, 0x22, 0x47, 0x69, 0x8E, 0x91, 0xB3, 0xD8, 0xDB}
    analyze_module(
        f"{base}\\warden_7C4ABC97B86494A2D91820785F3A1C87_decompressed.bin",
        "7C4ABC97 (known types)",
        known_types=known_7c
    )

    # Module 9A95D199
    analyze_module(
        f"{base}\\warden_9A95D19959AA88542116BE3639C0EB0F_decompressed.bin",
        "9A95D199 (unknown types)"
    )
