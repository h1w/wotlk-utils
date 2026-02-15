"""
Comprehensive dispatch chain analysis across ALL Warden modules.

For each module:
1. Find XOR sites (xor r8, [reg+4])
2. Classify: remap table vs dispatch chain
3. Extract types from ALL dispatch chains (union)
4. Extract types from remap tables (cross-reference for comparison)
5. Report gaps: types in remap but not in chains, and vice versa
"""

import struct
import zlib
import os
import sys
from capstone import *

DUMP_DIRS = [
    r"Z:\Games\wow 3.3.5a client\warden_dumps",
    r"C:\Users\bpqvg\source\repos\wotlk-utils\wotlk\docs\warden_dumps",
]

# Reference: module 7C4ABC97 has 9 known types (10 with 0x00=MODULE_USE)
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
        path = os.path.join(d, f"warden_{hash_str}_decrypted.bin")
        if os.path.exists(path):
            with open(path, 'rb') as f:
                raw = f.read()
            if len(raw) > 5 and raw[4] == 0x78:
                try:
                    return zlib.decompress(raw[4:])
                except zlib.error:
                    pass
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

def find_xor_sites(data, scan_start):
    """Find all 'xor r8, [reg+4]' + movzx sites."""
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
        # Find movzx within 10 bytes
        for j in range(i + 3, min(i + 13, len(data) - 2)):
            if data[j] == 0x0F and data[j+1] == 0xB6 and data[j+2] >= 0xC0:
                sites.append({'xor_off': i, 'movzx_off': j, 'after_movzx': j + 3})
                break
    return sites

def detect_remap_table(data, size, after_movzx):
    """Check if after movzx we have a remap table pattern (movzx byte [reg+large_disp] + jmp [reg*4+disp])."""
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
                # Check for jmp [reg*4+disp] nearby
                for j in range(i+1, min(i+8, len(insns))):
                    if insns[j].mnemonic == 'jmp' and len(insns[j].operands) == 1:
                        op = insns[j].operands[0]
                        if op.type == CS_OP_MEM and op.mem.scale == 4:
                            return True, remap_off, op.mem.disp
        # Also check for cmp+ja before the second movzx (range check)
    return False, 0, 0

def extract_dispatch_chain_types(data, size, after_movzx, xor_off, movzx_off):
    """Extract type IDs from cumulative sub/dec/cmp chain after movzx.
    Also pre-scans between XOR and movzx for register loads (mov r32, imm32)."""

    # Pre-scan for register values loaded between XOR and movzx
    pre_reg_vals = {}
    md_pre = Cs(CS_ARCH_X86, CS_MODE_32)
    md_pre.detail = True
    pre_code = data[xor_off:movzx_off]
    pre_insns = list(md_pre.disasm(pre_code, xor_off))
    for insn in pre_insns:
        if insn.mnemonic == 'mov' and len(insn.operands) == 2:
            dst, src = insn.operands
            if dst.type == CS_OP_REG and src.type == CS_OP_IMM:
                val = src.imm & 0xFFFFFFFF
                if val <= 0xFF:
                    pre_reg_vals[dst.reg] = val

    # Disassemble dispatch chain
    md = Cs(CS_ARCH_X86, CS_MODE_32)
    md.detail = True
    code = data[after_movzx:min(after_movzx + 500, size)]
    insns = list(md.disasm(code, after_movzx))

    types = set()
    accumulator = 0
    last_cmp_val = None
    split_targets = {}

    i = 0
    while i < min(100, len(insns)):
        insn = insns[i]

        if insn.mnemonic == 'cmp' and len(insn.operands) == 2:
            got_imm = False
            for op in insn.operands:
                if op.type == CS_OP_IMM:
                    val = op.imm & 0xFFFFFFFF
                    if val <= 0xFF:
                        last_cmp_val = val
                        got_imm = True
                elif op.type == CS_OP_REG and op.reg in pre_reg_vals:
                    last_cmp_val = pre_reg_vals[op.reg]
                    got_imm = True

            if i + 1 < len(insns) and last_cmp_val is not None:
                nx = insns[i + 1]
                if nx.mnemonic == 'je':
                    types.add(last_cmp_val)
                elif nx.mnemonic in ('jg', 'jge', 'jl', 'jle', 'ja', 'jb', 'jae', 'jbe'):
                    types.add(last_cmp_val)
                    if nx.operands and nx.operands[0].type == CS_OP_IMM:
                        split_targets[nx.operands[0].imm] = last_cmp_val
                elif nx.mnemonic == 'jne':
                    types.add(last_cmp_val)

        elif insn.mnemonic == 'sub' and len(insn.operands) == 2:
            if insn.operands[1].type == CS_OP_IMM:
                delta = insn.operands[1].imm & 0xFF
                accumulator += delta
                if i + 1 < len(insns):
                    nx = insns[i + 1]
                    if nx.mnemonic in ('je', 'jne'):
                        types.add(accumulator)

        elif insn.mnemonic == 'dec' and len(insn.operands) == 1:
            if insn.operands[0].type == CS_OP_REG:
                accumulator += 1
                if i + 1 < len(insns):
                    nx = insns[i + 1]
                    if nx.mnemonic in ('je', 'jne'):
                        types.add(accumulator)

        elif insn.mnemonic in ('push', 'call', 'ret', 'leave'):
            break

        i += 1

    # Follow split targets
    for target_addr, split_val in split_targets.items():
        if target_addr >= len(data):
            continue
        branch_code = data[target_addr:min(target_addr + 300, size)]
        branch_insns = list(md.disasm(branch_code, target_addr))
        branch_acc = 0
        for j in range(min(50, len(branch_insns))):
            bi = branch_insns[j]
            if bi.mnemonic == 'cmp' and len(bi.operands) == 2:
                for op in bi.operands:
                    if op.type == CS_OP_IMM and 0 < (op.imm & 0xFFFFFFFF) <= 0xFF:
                        cmp_val = op.imm & 0xFF
                        if j + 1 < len(branch_insns):
                            nb = branch_insns[j + 1]
                            if nb.mnemonic in ('je', 'jne', 'jg', 'jge', 'jl', 'jle', 'ja', 'jb'):
                                types.add(cmp_val)
                                if nb.mnemonic in ('jg', 'jge', 'jl', 'jle', 'ja', 'jb'):
                                    if nb.operands and nb.operands[0].type == CS_OP_IMM:
                                        # Recurse one more level of splits
                                        pass
                    elif op.type == CS_OP_REG and op.reg in pre_reg_vals:
                        cmp_val = pre_reg_vals[op.reg]
                        if j + 1 < len(branch_insns):
                            nb = branch_insns[j + 1]
                            if nb.mnemonic in ('je', 'jne', 'jg', 'jge', 'jl', 'jle'):
                                types.add(cmp_val)

            elif bi.mnemonic == 'sub' and len(bi.operands) == 2:
                if bi.operands[1].type == CS_OP_IMM:
                    delta = bi.operands[1].imm & 0xFF
                    branch_acc += delta
                    if j + 1 < len(branch_insns):
                        nb = branch_insns[j + 1]
                        if nb.mnemonic in ('je', 'jne'):
                            types.add(branch_acc)

            elif bi.mnemonic == 'dec' and len(bi.operands) == 1:
                branch_acc += 1
                if j + 1 < len(branch_insns):
                    nb = branch_insns[j + 1]
                    if nb.mnemonic in ('je', 'jne'):
                        types.add(branch_acc)

            elif bi.mnemonic in ('push', 'call', 'ret', 'leave'):
                break

    return types

def extract_remap_types(data, remap_off, max_type=0xFF):
    """Extract non-default type IDs from remap table."""
    if remap_off + max_type + 1 > len(data):
        return set()
    table = data[remap_off:remap_off + max_type + 1]
    counts = {}
    for b in table:
        counts[b] = counts.get(b, 0) + 1
    default_idx = max(counts.keys(), key=lambda k: counts[k])
    types = set()
    for i in range(len(table)):
        if table[i] != default_idx:
            types.add(i)
    return types

def analyze_module(hash_str, data):
    header = parse_header(data)
    if not header:
        return None
    scan_start = header['packedDataOff']

    sites = find_xor_sites(data, scan_start)

    chain_types_union = set()
    remap_types_union = set()
    chains = []
    remaps = []

    for site in sites:
        is_remap, remap_off, jtable_off = detect_remap_table(data, len(data), site['after_movzx'])

        if is_remap:
            rt = extract_remap_types(data, remap_off)
            remap_types_union |= rt
            remaps.append({
                'xor_off': site['xor_off'],
                'remap_off': remap_off,
                'jtable_off': jtable_off,
                'types': rt,
            })
        else:
            ct = extract_dispatch_chain_types(data, len(data), site['after_movzx'],
                                              site['xor_off'], site['movzx_off'])
            if ct:
                chain_types_union |= ct
                chains.append({
                    'xor_off': site['xor_off'],
                    'types': ct,
                })

    return {
        'hash': hash_str,
        'short': hash_str[:8],
        'size': len(data),
        'runtime_size': header['moduleSize'],
        'scan_start': scan_start,
        'xor_sites': len(sites),
        'chains': chains,
        'remaps': remaps,
        'chain_union': chain_types_union,
        'remap_union': remap_types_union,
    }

def main():
    # Collect all module hashes
    hashes = set()
    for d in DUMP_DIRS:
        if not os.path.exists(d):
            continue
        for f in os.listdir(d):
            if f.startswith('warden_') and '_decompressed' in f:
                parts = f.split('_')
                if len(parts) >= 3 and len(parts[1]) == 32:
                    hashes.add(parts[1])

    print(f"Found {len(hashes)} unique modules\n")

    all_results = []
    for h in sorted(hashes):
        data = load_module(h)
        if data is None:
            print(f"[SKIP] {h[:8]}: load failed")
            continue
        result = analyze_module(h, data)
        if result is None:
            print(f"[SKIP] {h[:8]}: invalid header")
            continue
        all_results.append(result)

    # Print results
    for r in all_results:
        print(f"\n{'='*80}")
        print(f"Module: {r['short']} (decompressed={r['size']}, runtime={r['runtime_size']})")
        print(f"{'='*80}")
        print(f"  XOR sites: {r['xor_sites']}")

        # Chains
        for i, c in enumerate(r['chains']):
            st = sorted(c['types'])
            print(f"  Chain #{i+1} (XOR@0x{c['xor_off']:04x}): {len(st)} types: "
                  f"{' '.join(f'0x{t:02X}' for t in st)}")

        # Remaps
        for i, rm in enumerate(r['remaps']):
            st = sorted(rm['types'])
            print(f"  Remap #{i+1} (XOR@0x{rm['xor_off']:04x}, table@0x{rm['remap_off']:04x}): "
                  f"{len(st)} types: {' '.join(f'0x{t:02X}' for t in st)}")

        chain_u = r['chain_union']
        remap_u = r['remap_union']

        print(f"\n  CHAIN UNION ({len(chain_u)} types): "
              f"{' '.join(f'0x{t:02X}' for t in sorted(chain_u))}")

        if remap_u:
            print(f"  REMAP UNION ({len(remap_u)} types): "
                  f"{' '.join(f'0x{t:02X}' for t in sorted(remap_u))}")

            in_remap_not_chain = remap_u - chain_u
            in_chain_not_remap = chain_u - remap_u
            if in_remap_not_chain:
                print(f"  ** IN REMAP BUT NOT CHAIN: "
                      f"{' '.join(f'0x{t:02X}' for t in sorted(in_remap_not_chain))}")
            if in_chain_not_remap:
                print(f"  ** IN CHAIN BUT NOT REMAP: "
                      f"{' '.join(f'0x{t:02X}' for t in sorted(in_chain_not_remap))}")

        # Special: check 7C4ABC97 against known types
        if r['short'] == '7C4ABC97':
            known = set(KNOWN_TYPES_7C4ABC97.keys())
            missing = known - chain_u
            if missing:
                print(f"  !! MISSING vs known: "
                      f"{' '.join(f'0x{t:02X}={KNOWN_TYPES_7C4ABC97[t]}' for t in sorted(missing))}")
            else:
                print(f"  All 9 known types found in chain union!")

    # Summary table
    print(f"\n\n{'='*80}")
    print(f"SUMMARY TABLE")
    print(f"{'='*80}")
    print(f"{'Module':<10} {'Size':>6} {'XOR':>4} {'Chains':>7} {'Remaps':>7} {'Chain#':>7} {'Remap#':>7} {'Gap':>4}")
    print(f"{'-'*10} {'-'*6} {'-'*4} {'-'*7} {'-'*7} {'-'*7} {'-'*7} {'-'*4}")

    for r in all_results:
        chain_n = len(r['chain_union'])
        remap_n = len(r['remap_union'])
        gap = len(r['remap_union'] - r['chain_union'])
        print(f"{r['short']:<10} {r['size']:>6} {r['xor_sites']:>4} "
              f"{len(r['chains']):>7} {len(r['remaps']):>7} "
              f"{chain_n:>7} {remap_n:>7} {gap:>4}")

    # Check if any module has types in remap but not in chains
    print(f"\n\n{'='*80}")
    print(f"MODULES WITH TYPES IN REMAP BUT NOT IN CHAIN UNION")
    print(f"{'='*80}")
    any_gap = False
    for r in all_results:
        gap = r['remap_union'] - r['chain_union']
        if gap:
            any_gap = True
            print(f"  {r['short']}: {len(gap)} missing types: "
                  f"{' '.join(f'0x{t:02X}' for t in sorted(gap))}")
    if not any_gap:
        print(f"  None! All remap types are covered by chain unions.")

if __name__ == '__main__':
    main()
