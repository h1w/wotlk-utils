"""
Analyze the remap table + jump table dispatcher pattern found in Warden modules.

Pattern:
  movzx eax, al                            ; zero-extend type byte
  cmp   eax, MAX_TYPE                       ; range check
  ja    default_handler                     ; out of range
  movzx eax, byte ptr [eax + REMAP_OFF]    ; remap table lookup
  jmp   dword ptr [eax*4 + JTABLE_OFF]     ; jump table dispatch

The remap table is a 256-byte array mapping type values to compact indices.
The jump table has handler pointers for each index.
"""

import struct
import sys
from capstone import *

def read_module(path):
    with open(path, 'rb') as f:
        return f.read()

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

def find_remap_dispatch(data, header):
    """
    Scan for the remap table + jump table pattern:
      movzx eax, byte ptr [eax/reg + OFFSET]   ; 0F B6 80 xx xx xx xx  (or other reg encodings)
      jmp   dword ptr [eax*4 + OFFSET]          ; FF 24 85 xx xx xx xx

    These two instructions should be adjacent or very close.
    """
    md = Cs(CS_ARCH_X86, CS_MODE_32)
    md.detail = True

    start = header['packedDataOff']
    code = data[start:]
    insns = list(md.disasm(code, start))

    results = []

    for i in range(len(insns) - 1):
        insn = insns[i]

        # Look for: movzx r32, byte ptr [r32 + disp32]
        if insn.mnemonic == 'movzx' and len(insn.operands) == 2:
            op_src = insn.operands[1]
            op_dst = insn.operands[0]
            if (op_src.type == CS_OP_MEM and op_src.size == 1 and
                op_dst.type == CS_OP_REG):
                remap_off = op_src.mem.disp
                if remap_off > 0x100:  # remap table should be at a significant offset
                    # Check next few instructions for jmp [reg*4 + disp]
                    for j in range(i+1, min(i+4, len(insns))):
                        next_insn = insns[j]
                        if next_insn.mnemonic == 'jmp' and len(next_insn.operands) == 1:
                            nop = next_insn.operands[0]
                            if nop.type == CS_OP_MEM and nop.mem.scale == 4:
                                jtable_off = nop.mem.disp
                                results.append({
                                    'movzx_addr': insn.address,
                                    'jmp_addr': next_insn.address,
                                    'remap_offset': remap_off,
                                    'jtable_offset': jtable_off,
                                    'movzx_str': f'{insn.mnemonic} {insn.op_str}',
                                    'jmp_str': f'{next_insn.mnemonic} {next_insn.op_str}',
                                })

    return results

def find_binary_cmp_dispatch(data, header):
    """
    Also scan for the binary-search cmp/je pattern:
      xor al, byte ptr [reg + 4]    ; XOR with xorByte
      movzx eax, al
      cmp eax, IMM                   ; compare with middle value
      jg/jl upper/lower              ; binary split
      je handler                     ; exact match
      cmp eax, IMM                   ; compare with type value
      je handler
      ...
    """
    md = Cs(CS_ARCH_X86, CS_MODE_32)
    md.detail = True

    start = header['packedDataOff']
    code = data[start:]
    insns = list(md.disasm(code, start))

    # Find xor al, byte ptr [reg + 4] followed by movzx eax, al
    results = []
    for i in range(len(insns) - 5):
        insn = insns[i]
        if (insn.mnemonic == 'xor' and 'al' in insn.op_str and
            'byte ptr' in insn.op_str and '+ 4]' in insn.op_str):
            # Found XOR with xorByte! Check what follows
            context = insns[i:i+20]
            cmp_values = []
            for ci in context:
                if ci.mnemonic in ('cmp', 'sub') and ci.op_count(CS_OP_IMM) > 0:
                    for op in ci.operands:
                        if op.type == CS_OP_IMM and 0 < (op.imm & 0xFF) < 0xFF:
                            cmp_values.append((ci.address, ci.mnemonic, ci.op_str, op.imm & 0xFF))
            if cmp_values:
                results.append({
                    'xor_addr': insn.address,
                    'xor_str': f'{insn.mnemonic} {insn.op_str}',
                    'comparisons': cmp_values,
                })

    return results

def extract_remap_table(data, remap_offset, max_type_val):
    """Read the remap table from the module binary."""
    if remap_offset + 256 > len(data):
        print(f"  WARNING: remap table at 0x{remap_offset:04x} exceeds module size")
        return None

    table = data[remap_offset:remap_offset + max_type_val + 1]
    # Find which type values map to non-default entries
    # The default (unmatched) entry is usually the highest index
    default_idx = table[max_type_val] if max_type_val < len(table) else None

    type_map = {}
    for i in range(min(256, len(table))):
        idx = table[i]
        if idx != default_idx or i == max_type_val:
            if idx not in type_map:
                type_map[idx] = []
            type_map[idx].append(i)

    return table, type_map, default_idx

def extract_jump_table(data, jtable_offset, num_entries):
    """Read the jump table from the module binary."""
    entries = []
    for i in range(num_entries):
        off = jtable_offset + i * 4
        if off + 4 > len(data):
            break
        addr = struct.unpack_from('<I', data, off)[0]
        entries.append(addr)
    return entries

def analyze_module(path, known_types=None, label=""):
    print(f"\n{'='*70}")
    print(f"Module: {label or path}")
    print(f"{'='*70}")

    data = read_module(path)
    header = parse_header(data)
    if not header:
        print("ERROR: Invalid header")
        return

    print(f"Size: {len(data)} bytes, runtime: {header['moduleSize']}")

    # Find remap+jump table dispatchers
    remap_results = find_remap_dispatch(data, header)
    print(f"\n--- Remap table + jump table dispatchers: {len(remap_results)} found ---")
    for r in remap_results:
        print(f"\n  0x{r['movzx_addr']:04x}: {r['movzx_str']}")
        print(f"  0x{r['jmp_addr']:04x}: {r['jmp_str']}")
        print(f"  Remap table at module offset: 0x{r['remap_offset']:04x}")
        print(f"  Jump table at module offset:  0x{r['jtable_offset']:04x}")

    # Find binary-search cmp dispatchers
    cmp_results = find_binary_cmp_dispatch(data, header)
    print(f"\n--- XOR + cmp/je dispatchers: {len(cmp_results)} found ---")
    for r in cmp_results:
        print(f"\n  XOR at 0x{r['xor_addr']:04x}: {r['xor_str']}")
        for addr, mn, ops, imm in r['comparisons']:
            name = ""
            if known_types and imm in known_types:
                name = f" *** KNOWN TYPE ***"
            print(f"    0x{addr:04x}: {mn} {ops}  (0x{imm:02x}){name}")

    # For remap dispatchers, try to extract the actual type mapping
    for r in remap_results:
        remap_off = r['remap_offset']
        jtable_off = r['jtable_offset']

        # We need to figure out the max type value - look for a preceding cmp
        md = Cs(CS_ARCH_X86, CS_MODE_32)
        md.detail = True
        # Disassemble a few bytes before the movzx to find the range check
        pre_start = max(0, r['movzx_addr'] - 20)
        pre_code = data[pre_start:r['movzx_addr'] + 7]
        max_type = 0xFF  # default
        for insn in md.disasm(pre_code, pre_start):
            if insn.mnemonic == 'cmp' and insn.op_count(CS_OP_IMM) > 0:
                for op in insn.operands:
                    if op.type == CS_OP_IMM and 0 < op.imm <= 0xFF:
                        max_type = op.imm

        print(f"\n--- Remap table analysis (max_type=0x{max_type:02x}) ---")
        result = extract_remap_table(data, remap_off, max_type)
        if result:
            table, type_map, default_idx = result
            print(f"  Default index: {default_idx}")

            # Show non-default type mappings
            active_types = []
            for idx in sorted(type_map.keys()):
                types = type_map[idx]
                if idx != default_idx:
                    for t in types:
                        active_types.append(t)
                        known_str = ""
                        if known_types and t in known_types:
                            known_str = " *** KNOWN ***"
                        print(f"  Type 0x{t:02x} -> index {idx}{known_str}")

            print(f"\n  Active types: {sorted([f'0x{t:02x}' for t in active_types])}")
            if known_types:
                found = set(active_types) & known_types
                missing = known_types - set(active_types)
                print(f"  Known types found: {sorted([f'0x{t:02x}' for t in found])}")
                if missing:
                    print(f"  Known types MISSING: {sorted([f'0x{t:02x}' for t in missing])}")

            # Show jump table
            num_indices = max(type_map.keys()) + 1 if type_map else 0
            jtable = extract_jump_table(data, jtable_off, num_indices + 1)
            print(f"\n  Jump table ({len(jtable)} entries at 0x{jtable_off:04x}):")
            for i, addr in enumerate(jtable):
                types_for_idx = [t for t, idx_list in [(tt, type_map.get(i, []))
                                 for tt in range(256) if i in type_map and tt in type_map[i]]
                                 ] if i in type_map else []
                # Simpler approach
                types_str = ""
                for t_idx, t_list in type_map.items():
                    if t_idx == i:
                        types_str = f" <- types: {[f'0x{t:02x}' for t in t_list]}"
                print(f"    [{i}] -> 0x{addr:08x}{types_str}")

    return remap_results, cmp_results

if __name__ == '__main__':
    base = r"C:\Users\bpqvg\source\repos\wotlk-utils\wotlk\docs\warden_dumps"

    # Module 7C4ABC97 - known types
    known_7c = {0x1F, 0x22, 0x47, 0x69, 0x8E, 0x91, 0xB3, 0xD8, 0xDB}
    analyze_module(
        f"{base}\\warden_7C4ABC97B86494A2D91820785F3A1C87_decompressed.bin",
        known_types=known_7c,
        label="7C4ABC97 (known types)"
    )

    # Module 9A95D199 - unknown types
    analyze_module(
        f"{base}\\warden_9A95D19959AA88542116BE3639C0EB0F_decompressed.bin",
        label="9A95D199 (unknown types)"
    )
