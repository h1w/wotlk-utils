"""
Deep analysis of ALL Warden modules:
1. Find every xor+movzx pattern
2. Classify what follows: remap table lookup? binary-search cmp/je? range check?
3. Extract actual type IDs from binary-search dispatchers
4. For modules without binary-search, investigate alternative dispatch patterns
"""

import struct
import zlib
import os
from capstone import *

DUMP_DIR = r"Z:\Games\wow 3.3.5a client\warden_dumps"
DOCS_DIR = r"C:\Users\bpqvg\source\repos\wotlk-utils\wotlk\docs\warden_dumps"

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

def classify_xor_movzx(insns, xor_idx, movzx_idx):
    """
    After xor+movzx, classify what follows:
    - 'remap_table': cmp + ja + movzx byte [reg+disp] + jmp [reg*4+disp]
    - 'binary_search': multiple cmp + je/jne with different imm values
    - 'range_check': single cmp + ja/jbe (just range validation)
    - 'unknown': can't classify
    """
    window = insns[movzx_idx+1 : min(movzx_idx+30, len(insns))]

    # Check for remap table pattern: cmp + ja, then movzx byte + jmp [reg*4+disp]
    has_range_cmp_ja = False
    has_remap_movzx = False
    has_jump_table = False
    range_val = None
    remap_off = None
    jtable_off = None

    cmp_je_pairs = []  # (cmp_value, je_target)
    cmp_jg_pairs = []  # binary search splits

    for i, insn in enumerate(window):
        # Range check: cmp reg, imm + ja/jbe
        if insn.mnemonic == 'cmp' and i + 1 < len(window):
            next_i = window[i+1]
            for op in insn.operands:
                if op.type == CS_OP_IMM:
                    val = op.imm & 0xFFFFFFFF
                    if next_i.mnemonic in ('ja', 'jbe', 'jb', 'jae'):
                        has_range_cmp_ja = True
                        range_val = val
                    elif next_i.mnemonic == 'je':
                        cmp_je_pairs.append(val)
                    elif next_i.mnemonic in ('jg', 'jge', 'jl', 'jle'):
                        cmp_jg_pairs.append(val)
                    elif next_i.mnemonic == 'jne':
                        cmp_je_pairs.append(val)  # jne = handler for NOT this type

        # Remap table: movzx r32, byte ptr [reg + large_disp]
        if insn.mnemonic == 'movzx' and len(insn.operands) == 2:
            src = insn.operands[1]
            if src.type == CS_OP_MEM and src.size == 1 and src.mem.disp > 0x100:
                has_remap_movzx = True
                remap_off = src.mem.disp

        # Jump table: jmp [reg*4 + disp]
        if insn.mnemonic == 'jmp' and len(insn.operands) == 1:
            op = insn.operands[0]
            if op.type == CS_OP_MEM and op.mem.scale == 4:
                has_jump_table = True
                jtable_off = op.mem.disp

    if has_remap_movzx and has_jump_table:
        return 'remap_table', {
            'range_val': range_val,
            'remap_off': remap_off,
            'jtable_off': jtable_off,
        }

    if len(cmp_je_pairs) >= 2 or (len(cmp_je_pairs) >= 1 and len(cmp_jg_pairs) >= 1):
        # Filter out duplicates and the range_val
        unique_vals = set(cmp_je_pairs + cmp_jg_pairs)
        if range_val is not None:
            unique_vals.discard(range_val)
        return 'binary_search', {
            'cmp_je': cmp_je_pairs,
            'cmp_jg': cmp_jg_pairs,
            'unique_types': sorted(unique_vals),
        }

    if has_range_cmp_ja and range_val is not None:
        return 'range_check', {'range_val': range_val}

    return 'unknown', {}

def find_all_xor_movzx_patterns(data, scan_start):
    """Find ALL xor reg8, mem8 + movzx patterns."""
    md = Cs(CS_ARCH_X86, CS_MODE_32)
    md.detail = True
    code = data[scan_start:]
    insns = list(md.disasm(code, scan_start))

    patterns = []
    for i in range(len(insns) - 5):
        insn = insns[i]
        if insn.mnemonic != 'xor':
            continue
        if len(insn.operands) != 2:
            continue
        dst, src = insn.operands
        if not (dst.type == CS_OP_REG and src.type == CS_OP_MEM and src.size == 1):
            continue

        # Check for movzx within next 5 instructions
        for j in range(i+1, min(i+6, len(insns))):
            if insns[j].mnemonic == 'movzx':
                kind, info = classify_xor_movzx(insns, i, j)
                patterns.append({
                    'xor_addr': insn.address,
                    'xor_str': f'{insn.mnemonic} {insn.op_str}',
                    'movzx_addr': insns[j].address,
                    'movzx_str': f'{insns[j].mnemonic} {insns[j].op_str}',
                    'kind': kind,
                    'info': info,
                })
                break

    return patterns

def find_full_binary_search_tree(data, scan_start, xor_addr):
    """
    Starting from the XOR instruction, follow the binary-search tree
    to find ALL type comparisons (not just those in a 40-instruction window).

    The pattern is:
      xor al, [reg+4]
      movzx eax, al
      cmp eax, MIDDLE     ; or cmp eax, reg (preloaded)
      jg upper_half
      je handler_MIDDLE
      cmp eax, LOW1
      je handler_LOW1
      cmp eax, LOW2
      je handler_LOW2
      ...
    upper_half:
      cmp eax, HIGH1
      je handler_HIGH1
      ...
    """
    md = Cs(CS_ARCH_X86, CS_MODE_32)
    md.detail = True

    # Disassemble a large window from xor_addr
    start_off = xor_addr
    if start_off >= len(data):
        return []
    code = data[start_off:min(start_off + 2048, len(data))]
    insns = list(md.disasm(code, start_off))

    # Collect ALL cmp+je/jne pairs, also track jg/jl for binary search splits
    type_values = set()
    split_values = set()  # values used in jg/jl (these are also valid types!)
    default_targets = set()  # addresses jumped to for "not found"

    for i in range(len(insns)):
        insn = insns[i]

        if insn.mnemonic in ('cmp', 'sub') and len(insn.operands) == 2:
            imm_val = None
            for op in insn.operands:
                if op.type == CS_OP_IMM:
                    imm_val = op.imm & 0xFF
                    if 0 < imm_val <= 0xFF:
                        break
                    else:
                        imm_val = None

            if imm_val is not None and i + 1 < len(insns):
                next_insn = insns[i+1]
                if next_insn.mnemonic == 'je':
                    type_values.add(imm_val)
                elif next_insn.mnemonic == 'jne':
                    type_values.add(imm_val)
                elif next_insn.mnemonic in ('jg', 'jge', 'jl', 'jle'):
                    split_values.add(imm_val)
                    type_values.add(imm_val)  # split values are also valid types
                elif next_insn.mnemonic in ('ja', 'jbe', 'jb', 'jae'):
                    # This is a range check, not a type comparison
                    pass

    return sorted(type_values), sorted(split_values)

def find_non_xor_dispatchers(data, scan_start):
    """
    For modules where no xor+movzx binary-search was found,
    look for alternative patterns:
    1. Direct movzx from packet buffer + cmp chains (no separate xor)
    2. switch/case tables
    3. Inline xor within the movzx source
    """
    md = Cs(CS_ARCH_X86, CS_MODE_32)
    md.detail = True
    code = data[scan_start:]
    insns = list(md.disasm(code, scan_start))

    results = []

    # Pattern 1: movzx + cmp chain (without preceding xor)
    for i in range(len(insns) - 10):
        insn = insns[i]
        if insn.mnemonic != 'movzx':
            continue
        if len(insn.operands) != 2:
            continue
        src = insn.operands[1]
        if src.size != 1:  # byte source
            continue

        # Count cmp+je/jne in next 30 instructions
        cmp_values = []
        for j in range(i+1, min(i+30, len(insns))):
            ni = insns[j]
            if ni.mnemonic in ('cmp', 'sub') and ni.op_count(CS_OP_IMM) > 0:
                for op in ni.operands:
                    if op.type == CS_OP_IMM:
                        val = op.imm & 0xFF
                        if 0 < val <= 0xFF:
                            cmp_values.append(val)

        if len(set(cmp_values)) >= 3:
            results.append({
                'type': 'movzx_cmp_chain',
                'addr': insn.address,
                'insn': f'{insn.mnemonic} {insn.op_str}',
                'cmp_values': sorted(set(cmp_values)),
            })

    # Pattern 2: sub reg, imm + cmp + je chains (type adjusted by subtraction)
    for i in range(len(insns) - 15):
        insn = insns[i]
        if insn.mnemonic != 'sub':
            continue
        if len(insn.operands) != 2:
            continue
        if insn.operands[0].type != CS_OP_REG or insn.operands[1].type != CS_OP_IMM:
            continue
        sub_val = insn.operands[1].imm & 0xFF

        # Count cmp/je chains after sub
        je_count = 0
        for j in range(i+1, min(i+20, len(insns))):
            ni = insns[j]
            if ni.mnemonic == 'je' or ni.mnemonic == 'jne':
                je_count += 1

        if je_count >= 3:
            results.append({
                'type': 'sub_cmp_chain',
                'addr': insn.address,
                'sub_val': sub_val,
                'je_count': je_count,
                'insn': f'{insn.mnemonic} {insn.op_str}',
            })

    return results

def analyze_module(hash_str, data):
    header = parse_header(data)
    if not header:
        print(f"  ERROR: Invalid header")
        return
    scan_start = header['packedDataOff']

    print(f"  Size: {len(data)} bytes, sections: {header['sectionDescCount']}")

    # Find all xor+movzx patterns and classify
    patterns = find_all_xor_movzx_patterns(data, scan_start)

    remap_patterns = [p for p in patterns if p['kind'] == 'remap_table']
    bsearch_patterns = [p for p in patterns if p['kind'] == 'binary_search']
    range_patterns = [p for p in patterns if p['kind'] == 'range_check']
    unknown_patterns = [p for p in patterns if p['kind'] == 'unknown']

    print(f"  XOR+MOVZX patterns: {len(patterns)} total")
    print(f"    Remap table:   {len(remap_patterns)}")
    print(f"    Binary search: {len(bsearch_patterns)}")
    print(f"    Range check:   {len(range_patterns)}")
    print(f"    Unknown:       {len(unknown_patterns)}")

    for p in remap_patterns:
        info = p['info']
        print(f"\n  REMAP @ 0x{p['xor_addr']:04x}: {p['xor_str']}")
        print(f"    remap_off=0x{info['remap_off']:04x}, jtable_off=0x{info['jtable_off']:04x}, "
              f"range=0x{info['range_val']:02x}" if info['range_val'] else "")

    for p in bsearch_patterns:
        info = p['info']
        print(f"\n  BINARY SEARCH @ 0x{p['xor_addr']:04x}: {p['xor_str']}")
        print(f"    cmp+je values: {[f'0x{v:02x}' for v in info['cmp_je']]}")
        print(f"    cmp+jg values: {[f'0x{v:02x}' for v in info['cmp_jg']]}")
        print(f"    unique types:  {[f'0x{v:02x}' for v in info['unique_types']]}")

        # Do full tree walk
        all_types, split_vals = find_full_binary_search_tree(data, scan_start, p['xor_addr'])
        print(f"    Full tree walk ({len(all_types)} types): {[f'0x{v:02x}' for v in all_types]}")
        if split_vals:
            print(f"    Split values (jg/jl pivots): {[f'0x{v:02x}' for v in split_vals]}")

    for p in range_patterns:
        info = p['info']
        print(f"\n  RANGE CHECK @ 0x{p['xor_addr']:04x}: {p['xor_str']}, range=0x{info['range_val']:02x}")

    for p in unknown_patterns:
        print(f"\n  UNKNOWN @ 0x{p['xor_addr']:04x}: {p['xor_str']}")

    # If no binary search found, look for alternative patterns
    if not bsearch_patterns:
        print(f"\n  === No binary-search dispatcher found! Searching alternatives... ===")
        alt_results = find_non_xor_dispatchers(data, scan_start)
        if alt_results:
            for r in alt_results:
                if r['type'] == 'movzx_cmp_chain':
                    print(f"    MOVZX+CMP chain @ 0x{r['addr']:04x}: {r['insn']}")
                    print(f"      values: {[f'0x{v:02x}' for v in r['cmp_values']]}")
                elif r['type'] == 'sub_cmp_chain':
                    print(f"    SUB+CMP chain @ 0x{r['addr']:04x}: {r['insn']}")
                    print(f"      sub_val=0x{r['sub_val']:02x}, {r['je_count']} je's")
        else:
            print(f"    No alternative dispatch patterns found either!")

        # Also dump all xor patterns for manual inspection
        if unknown_patterns:
            print(f"\n  === Unknown XOR+MOVZX patterns (manual inspection needed): ===")
            md = Cs(CS_ARCH_X86, CS_MODE_32)
            md.detail = True
            for p in unknown_patterns[:5]:  # Show first 5
                code_start = p['xor_addr']
                code = data[code_start:min(code_start+100, len(data))]
                print(f"\n    --- @ 0x{code_start:04x}: {p['xor_str']} ---")
                for insn in md.disasm(code, code_start):
                    if insn.address > code_start + 80:
                        break
                    raw = ' '.join(f'{b:02x}' for b in insn.bytes)
                    print(f"      0x{insn.address:04x}: {raw:24s} {insn.mnemonic:8s} {insn.op_str}")

def main():
    dirs = [DUMP_DIR, DOCS_DIR]
    hashes = set()
    for d in dirs:
        if not os.path.exists(d):
            continue
        for f in os.listdir(d):
            if f.startswith('warden_') and ('_decompressed' in f or '_decrypted' in f):
                parts = f.split('_')
                if len(parts) >= 3:
                    h = parts[1]
                    if len(h) == 32:
                        hashes.add(h)

    print(f"Found {len(hashes)} unique modules\n")

    for h in sorted(hashes):
        print(f"\n{'='*70}")
        print(f"Module: {h}")
        print(f"{'='*70}")

        data = load_module(h, dirs)
        if data is None:
            print(f"  SKIP: not found or decompress failed")
            continue

        analyze_module(h, data)

if __name__ == '__main__':
    main()
