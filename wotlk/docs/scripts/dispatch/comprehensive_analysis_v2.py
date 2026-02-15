"""
Comprehensive dispatch chain analysis v2 — improved to match C++ implementation.

Changes from v1:
- MOVZX search window: 18 bytes (was 10), matching C++ kMaxMovzxDist
- Chain walker: BFS queue with visited set (was recursive)
- Accumulator: per-branch tracking (was global)
- Branch limit: 16 (was unlimited)
- Instructions: CMP, SUB, DEC, TEST, unconditional JMP (was CMP/SUB/DEC only)
- Register tracking throughout scan (was pre-scan only)
- Scan limit: 400 bytes per branch (was 500/100 insns combined)
"""

import struct
import zlib
import os
import sys
from collections import deque
from capstone import *
from capstone.x86 import *

DUMP_DIRS = [
    r"Z:\Games\wow 3.3.5a client\warden_dumps",
    r"C:\Users\bpqvg\source\repos\wotlk-utils\wotlk\docs\warden_dumps",
]

# C++ constants matched
MAX_MOVZX_DIST = 18
CHAIN_SCAN_LEN = 400
MAX_BRANCHES = 16
MIN_CHAIN_TYPES = 3

# Reference: module 7C4ABC97
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
    """Find all 'xor r8, [reg+4]' + movzx sites with 18-byte window."""
    sites = []
    for i in range(scan_start, len(data) - 2):
        if data[i] != 0x32:
            continue
        modrm = data[i + 1]
        if not (0x40 <= modrm <= 0x7F):
            continue
        if (modrm & 7) == 4:  # SIB byte would follow
            continue
        if data[i + 2] != 0x04:  # disp8 = 4
            continue
        # Find movzx within MAX_MOVZX_DIST bytes (matching C++)
        for j in range(i + 3, min(i + 3 + MAX_MOVZX_DIST, len(data) - 2)):
            if data[j] == 0x0F and data[j+1] == 0xB6 and data[j+2] >= 0xC0:
                sites.append({'xor_off': i, 'movzx_off': j, 'after_movzx': j + 3})
                break
    return sites

def detect_remap_table(data, size, after_movzx):
    """Check if after movzx we have a remap table pattern."""
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
                            return True, remap_off, op.mem.disp
    return False, 0, 0

def extract_dispatch_chain_types_v2(data, size, after_movzx, xor_off, movzx_off):
    """BFS-based chain walker matching C++ ExtractDispatchChainTypes logic.

    Uses queue-based BFS with per-branch accumulator,
    visited set, and 16-branch limit.
    """
    md = Cs(CS_ARCH_X86, CS_MODE_32)
    md.detail = True

    # Pre-scan between XOR and MOVZX for register loads
    reg_vals = {}
    pre_code = data[xor_off:movzx_off]
    pre_insns = list(md.disasm(pre_code, xor_off))
    for insn in pre_insns:
        if insn.mnemonic == 'mov' and len(insn.operands) == 2:
            dst, src = insn.operands
            if dst.type == CS_OP_REG and src.type == CS_OP_IMM:
                val = src.imm & 0xFFFFFFFF
                if val <= 0xFF:
                    reg_vals[dst.reg] = val

    types = set()
    visited = set()

    # BFS queue: (start_offset_in_data, accumulator_value)
    queue = deque()
    queue.append((after_movzx, 0))
    branch_count = 0

    while queue and branch_count < MAX_BRANCHES:
        start, accum = queue.popleft()

        if start in visited:
            continue
        visited.add(start)
        branch_count += 1

        end = min(start + CHAIN_SCAN_LEN, size)
        if start >= size:
            continue

        code = data[start:end]
        insns = list(md.disasm(code, start))

        local_accum = accum
        local_regs = dict(reg_vals)  # copy for this branch

        for i, insn in enumerate(insns):
            if insn.address - start > CHAIN_SCAN_LEN:
                break

            # Track MOV r32, imm throughout scan
            if insn.mnemonic == 'mov' and len(insn.operands) == 2:
                dst, src = insn.operands
                if dst.type == CS_OP_REG and src.type == CS_OP_IMM:
                    val = src.imm & 0xFFFFFFFF
                    if val <= 0xFF:
                        local_regs[dst.reg] = val
                continue

            # Skip extra MOVZX instructions
            if insn.mnemonic == 'movzx':
                continue

            # CMP instruction
            if insn.mnemonic == 'cmp' and len(insn.operands) == 2:
                cmp_val = None
                op0, op1 = insn.operands

                # cmp r32, imm8/imm32
                if op0.type == CS_OP_REG and op1.type == CS_OP_IMM:
                    val = op1.imm & 0xFFFFFFFF
                    if val <= 0xFF:
                        cmp_val = val + local_accum

                # cmp r32, r32 (register indirect)
                elif op0.type == CS_OP_REG and op1.type == CS_OP_REG:
                    if op1.reg in local_regs:
                        cmp_val = local_regs[op1.reg] + local_accum
                    elif op0.reg in local_regs:
                        cmp_val = local_regs[op0.reg] + local_accum

                if cmp_val is not None and i + 1 < len(insns):
                    nx = insns[i + 1]
                    if nx.mnemonic == 'je':
                        types.add(cmp_val & 0xFF)
                    elif nx.mnemonic == 'jne':
                        types.add(cmp_val & 0xFF)
                        # Follow fall-through (next instruction after jne)
                        # JNE target goes to another branch
                        if nx.operands and nx.operands[0].type == CS_OP_IMM:
                            target = nx.operands[0].imm
                            if target < size and target not in visited:
                                queue.append((target, local_accum))
                    elif nx.mnemonic in ('jg', 'jge', 'ja', 'jae'):
                        # Greater-family: add type, follow branch target
                        types.add(cmp_val & 0xFF)
                        if nx.operands and nx.operands[0].type == CS_OP_IMM:
                            target = nx.operands[0].imm
                            if target < size and target not in visited:
                                queue.append((target, 0))  # reset accum for split
                    elif nx.mnemonic in ('jl', 'jle', 'jb', 'jbe'):
                        # Less-family: add type, follow branch target
                        types.add(cmp_val & 0xFF)
                        if nx.operands and nx.operands[0].type == CS_OP_IMM:
                            target = nx.operands[0].imm
                            if target < size and target not in visited:
                                queue.append((target, 0))  # reset accum for split
                continue

            # TEST instruction (for type 0x00 detection)
            if insn.mnemonic == 'test' and len(insn.operands) == 2:
                op0, op1 = insn.operands
                if op0.type == CS_OP_REG and op1.type == CS_OP_REG and op0.reg == op1.reg:
                    # test eax, eax → checks if zero → type = 0 + accum
                    if i + 1 < len(insns):
                        nx = insns[i + 1]
                        if nx.mnemonic in ('je', 'jne', 'jz', 'jnz'):
                            types.add(local_accum & 0xFF)
                continue

            # SUB instruction (accumulator chain)
            if insn.mnemonic == 'sub' and len(insn.operands) == 2:
                op0, op1 = insn.operands
                if op1.type == CS_OP_IMM:
                    delta = op1.imm & 0xFF
                    local_accum += delta
                    if i + 1 < len(insns):
                        nx = insns[i + 1]
                        if nx.mnemonic == 'je':
                            types.add(local_accum & 0xFF)
                        elif nx.mnemonic == 'jne':
                            types.add(local_accum & 0xFF)
                            if nx.operands and nx.operands[0].type == CS_OP_IMM:
                                target = nx.operands[0].imm
                                if target < size and target not in visited:
                                    queue.append((target, local_accum))
                continue

            # DEC instruction (accumulator += 1)
            if insn.mnemonic == 'dec' and len(insn.operands) == 1:
                if insn.operands[0].type == CS_OP_REG:
                    local_accum += 1
                    if i + 1 < len(insns):
                        nx = insns[i + 1]
                        if nx.mnemonic == 'je':
                            types.add(local_accum & 0xFF)
                        elif nx.mnemonic == 'jne':
                            types.add(local_accum & 0xFF)
                            if nx.operands and nx.operands[0].type == CS_OP_IMM:
                                target = nx.operands[0].imm
                                if target < size and target not in visited:
                                    queue.append((target, local_accum))
                continue

            # Unconditional JMP — follow target
            if insn.mnemonic == 'jmp' and len(insn.operands) == 1:
                op = insn.operands[0]
                if op.type == CS_OP_IMM:
                    target = op.imm
                    if target < size and target not in visited:
                        queue.append((target, local_accum))
                break  # end this branch

            # Stop on function boundary
            if insn.mnemonic in ('push', 'call', 'ret', 'leave', 'int3'):
                break

            # Stop on stack frame setup
            if insn.mnemonic == 'lea' and len(insn.operands) == 2:
                dst = insn.operands[0]
                if dst.type == CS_OP_REG and dst.reg in (X86_REG_ESP, X86_REG_EBP):
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
            ct = extract_dispatch_chain_types_v2(data, len(data), site['after_movzx'],
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
    hashes = set()
    for d in DUMP_DIRS:
        if not os.path.exists(d):
            print(f"[WARN] Directory not found: {d}")
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

    # Detailed per-module output
    for r in all_results:
        print(f"\n{'='*80}")
        print(f"Module: {r['short']} (decompressed={r['size']}, runtime={r['runtime_size']})")
        print(f"{'='*80}")
        print(f"  XOR sites: {r['xor_sites']}")

        for i, c in enumerate(r['chains']):
            st = sorted(c['types'])
            print(f"  Chain #{i+1} (XOR@0x{c['xor_off']:04x}): {len(st)} types: "
                  f"{' '.join(f'0x{t:02X}' for t in st)}")

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
                print(f"  IN REMAP BUT NOT CHAIN ({len(in_remap_not_chain)}): "
                      f"{' '.join(f'0x{t:02X}' for t in sorted(in_remap_not_chain))}")
            if in_chain_not_remap:
                print(f"  IN CHAIN BUT NOT REMAP ({len(in_chain_not_remap)}): "
                      f"{' '.join(f'0x{t:02X}' for t in sorted(in_chain_not_remap))}")

        # Validate 7C4ABC97
        if r['short'] == '7C4ABC97':
            known = set(KNOWN_TYPES_7C4ABC97.keys())
            missing = known - chain_u
            if missing:
                print(f"  !! MISSING vs known: "
                      f"{' '.join(f'0x{t:02X}={KNOWN_TYPES_7C4ABC97[t]}' for t in sorted(missing))}")
            else:
                print(f"  OK: All 9 known types found in chain union!")

    # Summary table
    print(f"\n\n{'='*80}")
    print(f"SUMMARY TABLE (v2 — improved movzx window + BFS chain walker)")
    print(f"{'='*80}")
    print(f"{'Module':<10} {'Size':>6} {'XOR':>4} {'Chains':>7} {'Remaps':>7} {'Chain#':>7} {'Remap#':>7} {'Gap':>4}")
    print(f"{'-'*10} {'-'*6} {'-'*4} {'-'*7} {'-'*7} {'-'*7} {'-'*7} {'-'*4}")

    total_full = 0
    total_partial = 0
    total_none = 0

    for r in all_results:
        chain_n = len(r['chain_union'])
        remap_n = len(r['remap_union'])
        gap = len(r['remap_union'] - r['chain_union'])
        marker = ""
        if chain_n >= 9:
            marker = " <-- FULL"
            total_full += 1
        elif chain_n >= 4:
            marker = " <-- PARTIAL"
            total_partial += 1
        elif chain_n == 0 and r['xor_sites'] == 0:
            marker = " <-- NO XOR"
            total_none += 1
        elif chain_n == 0:
            marker = " <-- REMAP ONLY"
            total_none += 1

        print(f"{r['short']:<10} {r['size']:>6} {r['xor_sites']:>4} "
              f"{len(r['chains']):>7} {len(r['remaps']):>7} "
              f"{chain_n:>7} {remap_n:>7} {gap:>4}{marker}")

    print(f"\nFull (>=9 types): {total_full}/{len(all_results)}")
    print(f"Partial (4-8):    {total_partial}/{len(all_results)}")
    print(f"None (0):         {total_none}/{len(all_results)}")

    # Cross-reference: for modules with both chains and remaps, check overlap
    print(f"\n\n{'='*80}")
    print(f"CROSS-REFERENCE: CHAIN vs REMAP TYPE OVERLAP")
    print(f"{'='*80}")
    for r in all_results:
        if r['chain_union'] and r['remap_union']:
            overlap = r['chain_union'] & r['remap_union']
            chain_only = r['chain_union'] - r['remap_union']
            remap_only = r['remap_union'] - r['chain_union']
            print(f"  {r['short']}: chain={len(r['chain_union'])}, remap={len(r['remap_union'])}, "
                  f"overlap={len(overlap)}, chain_only={len(chain_only)}, remap_only={len(remap_only)}")
            if chain_only:
                print(f"    Chain-only: {' '.join(f'0x{t:02X}' for t in sorted(chain_only))}")

    # For modules with ONLY remaps, check if remap types <= 10 (potential dispatch types)
    print(f"\n\n{'='*80}")
    print(f"REMAP-ONLY MODULES: type distribution analysis")
    print(f"{'='*80}")
    for r in all_results:
        if not r['chain_union'] and r['remap_union']:
            print(f"  {r['short']}: {len(r['remap_union'])} remap types")
            # Group remap entries by their target handler index
            for rm in r['remaps']:
                remap_off = rm['remap_off']
                if remap_off + 256 <= len(data):
                    pass  # Would need data reference for deeper analysis

if __name__ == '__main__':
    main()
