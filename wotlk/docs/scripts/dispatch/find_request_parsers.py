"""
Find the REQUEST PARSER in all Warden modules.

Strategy:
1. Raw byte scan for ALL 'xor r8, byte ptr [reg + 4]' instructions (opcode 32, ModRM 40-7F, disp8=4)
2. Disassemble from each XOR offset using capstone
3. Classify what follows:
   - Remap table (movzx byte [reg + large_disp] + jmp [reg*4 + disp]) -> RESPONSE BUILDER, skip
   - Cumulative sub chain (sub + je/jne) -> REQUEST PARSER, extract accumulated types
   - Binary-search cmp/je chain -> REQUEST PARSER, extract comparison values
4. For request parsers, follow BOTH halves of binary splits (jg/jle targets)
"""

import struct
import zlib
import os
from capstone import *

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
        path = os.path.join(d, f"warden_{hash_str}_inmemory.bin")
        if os.path.exists(path):
            with open(path, 'rb') as f:
                return f.read()
    return None

def raw_scan_xor_byte_reg4(data):
    """Find all 'xor r8, byte ptr [reg + 4]' by raw byte scan.
    Encoding: 32 [ModRM] 04
    ModRM: mod=01 (disp8), reg=any, rm=any except 4(SIB) and 5(EBP needs special)
    Actually rm=5 with mod=01 is [EBP+disp8], which is valid.
    rm=4 with mod=01 means SIB follows, so 32 [0x44|0x4C|...] would be wrong.
    """
    results = []
    for off in range(len(data) - 2):
        if data[off] != 0x32:
            continue
        modrm = data[off + 1]
        # mod=01 -> 0x40-0x7F
        if not (0x40 <= modrm <= 0x7F):
            continue
        # rm != 4 (SIB)
        if (modrm & 7) == 4:
            continue
        # disp8 must be 4
        if data[off + 2] != 4:
            continue
        regs = ['eax', 'ecx', 'edx', 'ebx', 'esp', 'ebp', 'esi', 'edi']
        dst_reg8 = ['al', 'cl', 'dl', 'bl', 'ah', 'ch', 'dh', 'bh'][(modrm >> 3) & 7]
        src_base = regs[modrm & 7]
        results.append((off, dst_reg8, src_base))
    return results

def disasm_from(data, offset, count=80):
    """Disassemble 'count' instructions starting from 'offset'."""
    md = Cs(CS_ARCH_X86, CS_MODE_32)
    md.detail = True
    code = data[offset:min(offset + count * 15, len(data))]  # ~15 bytes max per insn
    return list(md.disasm(code, offset))

def classify_dispatcher(insns, xor_idx):
    """After the XOR instruction, classify the dispatch pattern.
    Returns ('remap_table', info), ('sub_chain', types), ('binary_search', types), or ('unknown', [])
    """
    # Find movzx within first 6 instructions after XOR
    movzx_idx = None
    for j in range(xor_idx + 1, min(xor_idx + 6, len(insns))):
        if insns[j].mnemonic == 'movzx':
            movzx_idx = j
            break
    if movzx_idx is None:
        return 'no_movzx', {}

    # Look at what follows movzx
    window = insns[movzx_idx + 1:]

    # Check for remap table: cmp + ja then movzx byte [reg+large_disp] then jmp [reg*4+disp]
    for i in range(min(20, len(window))):
        insn = window[i]
        if insn.mnemonic == 'movzx' and len(insn.operands) == 2:
            src = insn.operands[1]
            if src.type == CS_OP_MEM and src.size == 1 and src.mem.disp > 0x100:
                # Check for jmp [reg*4+disp] nearby
                for j in range(i+1, min(i+5, len(window))):
                    if window[j].mnemonic == 'jmp' and len(window[j].operands) == 1:
                        op = window[j].operands[0]
                        if op.type == CS_OP_MEM and op.mem.scale == 4:
                            return 'remap_table', {
                                'remap_off': src.mem.disp,
                                'jtable_off': op.mem.disp,
                            }

    # Not a remap table — look for cumulative sub/cmp chain
    # This IS the request parser!
    return 'dispatch_chain', extract_dispatch_chain(insns, movzx_idx)

def extract_dispatch_chain(insns, movzx_idx):
    """Extract type IDs from a cumulative sub/dec/cmp chain after movzx.

    Pattern types:
    1. cmp eax, IMM; jg upper; je handler  — binary split + exact match
    2. sub eax, IMM; je handler             — cumulative subtraction
    3. dec eax; je handler                  — same as sub eax, 1
    4. cmp eax, IMM; jle lower; ...         — binary split

    We need to follow BOTH branches of binary splits.
    """
    types = set()
    split_targets = {}  # address -> cumulative_value at that point

    # Start after movzx
    accumulator = 0
    last_cmp_val = None
    i = movzx_idx + 1

    # First pass: linear scan for sub/dec/cmp chain
    while i < min(movzx_idx + 100, len(insns)):
        insn = insns[i]

        if insn.mnemonic == 'cmp' and len(insn.operands) == 2:
            for op in insn.operands:
                if op.type == CS_OP_IMM:
                    val = op.imm & 0xFFFFFFFF
                    if val <= 0xFF:
                        last_cmp_val = val
                    elif op.type == CS_OP_REG:
                        # cmp eax, ecx — the register might hold a preloaded value
                        # We can't know the value statically, but it's often the split point
                        pass

            # Check next instruction for branch type
            if i + 1 < len(insns):
                next_insn = insns[i + 1]
                if next_insn.mnemonic == 'je':
                    if last_cmp_val is not None:
                        types.add(last_cmp_val)
                elif next_insn.mnemonic in ('jg', 'jge'):
                    # Binary split: types > val go to target
                    if last_cmp_val is not None:
                        types.add(last_cmp_val)  # The split value itself is also a type
                        target = next_insn.operands[0].imm if next_insn.operands[0].type == CS_OP_IMM else None
                        if target:
                            split_targets[target] = last_cmp_val
                elif next_insn.mnemonic in ('jle', 'jl'):
                    # Types <= val go to target
                    if last_cmp_val is not None:
                        types.add(last_cmp_val)
                        target = next_insn.operands[0].imm if next_insn.operands[0].type == CS_OP_IMM else None
                        if target:
                            split_targets[target] = last_cmp_val

        elif insn.mnemonic == 'sub' and len(insn.operands) == 2:
            if insn.operands[1].type == CS_OP_IMM:
                delta = insn.operands[1].imm & 0xFF
                accumulator += delta
                # Check next for je/jne
                if i + 1 < len(insns):
                    next_insn = insns[i + 1]
                    if next_insn.mnemonic == 'je':
                        types.add(accumulator)
                    elif next_insn.mnemonic == 'jne':
                        types.add(accumulator)

        elif insn.mnemonic == 'dec' and len(insn.operands) == 1:
            if insn.operands[0].type == CS_OP_REG:
                accumulator += 1
                if i + 1 < len(insns):
                    next_insn = insns[i + 1]
                    if next_insn.mnemonic == 'je':
                        types.add(accumulator)
                    elif next_insn.mnemonic == 'jne':
                        types.add(accumulator)

        elif insn.mnemonic in ('push', 'call', 'ret', 'leave'):
            # End of dispatch chain
            break

        i += 1

    # Second pass: try to follow split targets
    # For each split target, try to find more types by disassembling from there
    for target_addr, split_val in split_targets.items():
        if target_addr < (insns[-1].address if insns else 0) + 100:
            # Find instruction index at target
            target_insns = None
            for idx, insn in enumerate(insns):
                if insn.address == target_addr:
                    target_insns = insns[idx:]
                    break

            if target_insns is None:
                # Target might be outside our disassembly window
                # Try to disassemble from target
                if target_addr < len(data_global):
                    target_insns_list = disasm_from(data_global, target_addr, 50)
                    if target_insns_list:
                        target_insns = target_insns_list

            if target_insns:
                branch_acc = 0
                for j in range(min(50, len(target_insns))):
                    bi = target_insns[j]
                    if bi.mnemonic == 'cmp' and len(bi.operands) == 2:
                        for op in bi.operands:
                            if op.type == CS_OP_IMM and 0 < (op.imm & 0xFF) <= 0xFF:
                                cmp_val = op.imm & 0xFF
                                if j + 1 < len(target_insns):
                                    nb = target_insns[j + 1]
                                    if nb.mnemonic in ('je', 'jne'):
                                        types.add(cmp_val)
                                    elif nb.mnemonic in ('jg', 'jge', 'jl', 'jle'):
                                        types.add(cmp_val)

                    elif bi.mnemonic == 'sub' and len(bi.operands) == 2:
                        if bi.operands[1].type == CS_OP_IMM:
                            delta = bi.operands[1].imm & 0xFF
                            branch_acc += delta
                            if j + 1 < len(target_insns):
                                nb = target_insns[j + 1]
                                if nb.mnemonic in ('je', 'jne'):
                                    types.add(branch_acc)

                    elif bi.mnemonic == 'dec' and len(bi.operands) == 1:
                        branch_acc += 1
                        if j + 1 < len(target_insns):
                            nb = target_insns[j + 1]
                            if nb.mnemonic in ('je', 'jne'):
                                types.add(branch_acc)

                    elif bi.mnemonic in ('push', 'call', 'ret', 'leave'):
                        break

    return {
        'types': sorted(types),
        'splits': split_targets,
    }

# Global for access in nested functions
data_global = None

def analyze_module(hash_str, data, short_hash):
    global data_global
    data_global = data

    known = KNOWN_TYPES.get(short_hash, {})
    dfs = DFS_LEARNED.get(short_hash[:6], {})

    # Find all XOR instructions by raw byte scan
    xor_hits = raw_scan_xor_byte_reg4(data)
    print(f"  Raw XOR scan: {len(xor_hits)} hits")

    request_parser_types = set()
    remap_count = 0
    chain_count = 0

    for off, dst_r8, src_base in xor_hits:
        # Disassemble from XOR offset
        insns = disasm_from(data, off, 80)
        if not insns:
            continue

        # Find XOR in disassembled output (should be first instruction)
        xor_idx = None
        for idx, insn in enumerate(insns):
            if insn.address == off and insn.mnemonic == 'xor':
                xor_idx = idx
                break

        if xor_idx is None:
            continue

        kind, info = classify_dispatcher(insns, xor_idx)

        if kind == 'remap_table':
            remap_count += 1
            print(f"    XOR @ 0x{off:04x}: xor {dst_r8}, [{src_base}+4] -> REMAP TABLE "
                  f"(remap=0x{info['remap_off']:04x}, jtable=0x{info['jtable_off']:04x})")

        elif kind == 'dispatch_chain':
            types = info.get('types', [])
            if len(types) >= 3:  # Likely a real dispatcher
                chain_count += 1
                request_parser_types.update(types)

                known_str = ""
                for t in types:
                    if t in known:
                        known_str += f" [0x{t:02x}={known[t]}]"
                    if t in dfs:
                        known_str += f" [0x{t:02x}=DFS:{dfs[t]}]"

                print(f"    XOR @ 0x{off:04x}: xor {dst_r8}, [{src_base}+4] -> DISPATCH CHAIN "
                      f"({len(types)} types): {[f'0x{t:02x}' for t in types]}{known_str}")

                # Show disassembly for verification
                for insn in insns[xor_idx:xor_idx+30]:
                    raw = ' '.join(f'{b:02x}' for b in insn.bytes)
                    print(f"      0x{insn.address:04x}: {raw:24s} {insn.mnemonic:8s} {insn.op_str}")
            else:
                print(f"    XOR @ 0x{off:04x}: xor {dst_r8}, [{src_base}+4] -> CHAIN "
                      f"(only {len(types)} types, skipping)")

        elif kind == 'no_movzx':
            print(f"    XOR @ 0x{off:04x}: xor {dst_r8}, [{src_base}+4] -> NO MOVZX found")

    print(f"\n  Summary: {remap_count} remap tables, {chain_count} dispatch chains")
    if request_parser_types:
        print(f"  REQUEST PARSER types: {[f'0x{t:02x}' for t in sorted(request_parser_types)]}")

        if known:
            found = set(known.keys()) & request_parser_types
            missing = set(known.keys()) - request_parser_types
            extra = request_parser_types - set(known.keys())
            print(f"  VALIDATION: found={[f'0x{t:02x}={known[t]}' for t in sorted(found)]}")
            if missing:
                print(f"  MISSING: {[f'0x{t:02x}={known[t]}' for t in sorted(missing)]}")
            if extra:
                print(f"  EXTRA (unknown): {[f'0x{t:02x}' for t in sorted(extra)]}")

def main():
    dirs = [DUMP_DIR, DOCS_DIR]
    hashes = set()
    for d in dirs:
        if not os.path.exists(d):
            continue
        for f in os.listdir(d):
            if f.startswith('warden_') and ('_decompressed' in f or '_decrypted' in f or '_inmemory' in f):
                parts = f.split('_')
                if len(parts) >= 3 and len(parts[1]) == 32:
                    hashes.add(parts[1])

    print(f"Scanning {len(hashes)} modules for request parsers\n")

    for h in sorted(hashes):
        short = h[:8]
        print(f"\n{'='*70}")
        print(f"Module: {short}")
        print(f"{'='*70}")

        data = load_module(h, dirs)
        if data is None:
            print("  SKIP")
            continue

        analyze_module(h, data, short)

if __name__ == '__main__':
    main()
