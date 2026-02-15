"""
Comprehensive Warden module check type extractor v2.

Key improvements over find_request_parsers.py:
1. Register value tracking (mov ecx, 0x8E; cmp eax, ecx -> value is 0x8E)
2. Recursive branch following (jg/jle splits -> follow BOTH subtrees)
3. cmp+jne handling (fall-through = handler for the compared value)
4. Proper accumulator tracking for sub/dec chains
5. Better remap vs dispatch classification
6. Handles modules with alternative dispatcher patterns
"""

import struct
import zlib
import os
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


def decompress_module(decrypted_data):
    if len(decrypted_data) < 5:
        return None
    decomp_size = struct.unpack_from('<I', decrypted_data, 0)[0]
    if decomp_size == 0 or decomp_size > 4 * 1024 * 1024:
        return None
    if decrypted_data[4] != 0x78:
        return None
    try:
        return zlib.decompress(decrypted_data[4:])
    except zlib.error:
        return None


def load_module(hash_str):
    for suffix in ('_decompressed.bin', '_decrypted.bin'):
        path = os.path.join(DOCS_DIR, f"warden_{hash_str}{suffix}")
        if os.path.exists(path):
            with open(path, 'rb') as f:
                data = f.read()
            if suffix == '_decrypted.bin':
                data = decompress_module(data)
            return data
    return None


def disasm_at(data, offset, max_insns=120):
    """Disassemble up to max_insns instructions starting at offset."""
    end = min(offset + max_insns * 15, len(data))
    code = data[offset:end]
    return list(md.disasm(code, offset))


def get_jump_target(insn):
    """Get the target address of a conditional/unconditional jump."""
    if len(insn.operands) == 1 and insn.operands[0].type == CS_OP_IMM:
        return insn.operands[0].imm
    return None


def raw_scan_xor_byte_reg_disp(data, disp_val=4):
    """Find all 'xor r8, byte ptr [reg + disp8]' by raw byte scan.
    Encoding: 32 [ModRM] disp8
    ModRM: mod=01 (disp8), reg=any, rm=any except 4(SIB)
    """
    results = []
    for off in range(len(data) - 2):
        if data[off] != 0x32:
            continue
        modrm = data[off + 1]
        if not (0x40 <= modrm <= 0x7F):
            continue
        if (modrm & 7) == 4:  # SIB byte follows
            continue
        if data[off + 2] != disp_val:
            continue
        regs8 = ['al', 'cl', 'dl', 'bl', 'ah', 'ch', 'dh', 'bh']
        regs32 = ['eax', 'ecx', 'edx', 'ebx', 'esp', 'ebp', 'esi', 'edi']
        dst_r8 = regs8[(modrm >> 3) & 7]
        src_base = regs32[modrm & 7]
        results.append((off, dst_r8, src_base))
    return results


def detect_remap_table(insns, movzx_idx):
    """Check if the code after movzx is a remap table pattern.
    Pattern: [add eax, shift] [cmp eax, max; ja default] movzx byte [reg + large_disp] jmp [reg*4 + disp]
    Returns (remap_off, jtable_off, shift, max_type) or None.
    """
    window = insns[movzx_idx + 1:movzx_idx + 25]

    # Detect shift (add eax, -N or sub eax, N) and max_type (cmp eax, N; ja)
    shift = 0
    max_type = 0xFF
    for i, insn in enumerate(window):
        if insn.mnemonic == 'add' and len(insn.operands) == 2:
            if insn.operands[1].type == CS_OP_IMM:
                # Python capstone returns signed int: add eax, -0x71 → val = -113
                val = insn.operands[1].imm
                if val < 0:
                    shift = (-val) & 0xFF
                elif val > 0x80000000:  # unsigned 2's complement fallback
                    shift = (0x100000000 - val) & 0xFF
        if insn.mnemonic == 'sub' and len(insn.operands) == 2:
            if insn.operands[0].type == CS_OP_REG and insn.operands[1].type == CS_OP_IMM:
                val = insn.operands[1].imm & 0xFF
                if 0 < val < 0x80:
                    shift = val
        if insn.mnemonic == 'cmp' and len(insn.operands) == 2:
            if insn.operands[1].type == CS_OP_IMM:
                val = insn.operands[1].imm
                if val < 0:
                    val = val & 0xFFFFFFFF
                if 0x80 <= (val & 0xFF) <= 0xFF and val <= 0xFF:
                    max_type = val

    for i, insn in enumerate(window):
        if insn.mnemonic == 'movzx' and len(insn.operands) == 2:
            src = insn.operands[1]
            if src.type == CS_OP_MEM and src.size == 1:
                disp = src.mem.disp
                if disp < 0:
                    disp = disp & 0xFFFFFFFF
                if disp > 0x100:  # large displacement = table offset
                    # Look for jmp [reg*4 + disp] nearby
                    for j in range(i + 1, min(i + 8, len(window))):
                        w = window[j]
                        if w.mnemonic == 'jmp' and len(w.operands) == 1:
                            op = w.operands[0]
                            if op.type == CS_OP_MEM and op.mem.scale == 4:
                                jt_disp = op.mem.disp
                                if jt_disp < 0:
                                    jt_disp = jt_disp & 0xFFFFFFFF
                                return (disp, jt_disp, shift, max_type)
    return None


def get_remap_singletons_pairs(data, remap_off, shift, max_type, verbose=False):
    """Get singletons and pairs from a remap table, grouped by INDEX VALUE.
    Returns (singletons_set, pairs_list, all_non_default_set).
    Groups by the raw index byte in the remap table (not handler address).
    """
    if remap_off + max_type + 1 > len(data):
        return set(), [], set()

    table = data[remap_off:remap_off + max_type + 1]

    # Count occurrences of each index value
    index_counts = {}
    for idx in table:
        index_counts[idx] = index_counts.get(idx, 0) + 1

    # Default = most common index value
    default_idx = max(index_counts.keys(), key=lambda k: index_counts[k])

    # Group type bytes by index value
    groups = {}
    for i in range(len(table)):
        idx = table[i]
        if idx != default_idx:
            if idx not in groups:
                groups[idx] = []
            groups[idx].append(i)

    singletons = set()
    pairs = []
    all_non_default = set()

    for idx, members in sorted(groups.items(), key=lambda x: len(x[1])):
        real_members = [(m + shift) & 0xFF for m in members]
        all_non_default.update(real_members)
        if len(members) == 1:
            singletons.add(real_members[0])
        elif len(members) == 2:
            pairs.append(sorted(real_members))

    if verbose:
        print(f"    remap @ 0x{remap_off:04x}, shift=0x{shift:02x}, max=0x{max_type:02x}")
        print(f"    default_idx={default_idx}, {index_counts[default_idx]}/{max_type+1} entries are default")
        print(f"    {len(groups)} non-default groups, {len(singletons)} singletons, {len(pairs)} pairs")

    return singletons, pairs, all_non_default


def is_remap_table(insns, movzx_idx):
    """Quick check: is this a remap table?"""
    return detect_remap_table(insns, movzx_idx) is not None


def extract_types_recursive(data, start_offset, initial_reg_vals=None, verbose=False):
    """
    Walk the dispatch tree starting after movzx, extract all type IDs.
    Uses recursive branch following for binary search trees.
    initial_reg_vals: dict of register -> value from pre-scan (e.g., mov ecx, 0x8E before movzx)
    """
    types = set()
    visited = set()

    def walk(offset, accumulator=0, depth=0, label="root", extra_regs=None):
        if depth > 12:
            return
        if offset in visited:
            return
        visited.add(offset)

        insns = disasm_at(data, offset, 80)
        if not insns:
            return

        acc = accumulator
        reg_vals = dict(initial_reg_vals) if initial_reg_vals and depth == 0 else {}
        if extra_regs:
            reg_vals.update(extra_regs)

        i = 0
        while i < len(insns):
            insn = insns[i]

            # Track mov reg, imm
            if insn.mnemonic == 'mov' and len(insn.operands) == 2:
                dst, src = insn.operands
                if dst.type == CS_OP_REG and src.type == CS_OP_IMM:
                    val = src.imm & 0xFFFFFFFF
                    if val <= 0xFF:
                        reg_vals[dst.reg] = val

            # Skip movzx (doesn't affect dispatch logic)
            elif insn.mnemonic == 'movzx':
                pass

            # Handle cmp
            elif insn.mnemonic == 'cmp' and len(insn.operands) == 2:
                cmp_val = None
                op1, op2 = insn.operands

                # cmp reg, imm
                if op2.type == CS_OP_IMM:
                    val = op2.imm & 0xFFFFFFFF
                    if val <= 0xFF:
                        cmp_val = val

                # cmp reg, reg (use tracked value)
                elif op1.type == CS_OP_REG and op2.type == CS_OP_REG:
                    if op2.reg in reg_vals:
                        cmp_val = reg_vals[op2.reg]
                    elif op1.reg in reg_vals:
                        cmp_val = reg_vals[op1.reg]

                # cmp eax, ecx where ecx was loaded
                elif op2.type == CS_OP_REG and op2.reg in reg_vals:
                    cmp_val = reg_vals[op2.reg]

                if cmp_val is not None and i + 1 < len(insns):
                    ni = insns[i + 1]

                    if ni.mnemonic == 'je':
                        types.add(cmp_val)
                        if verbose:
                            print(f"    {'  '*depth}[{label}] cmp 0x{cmp_val:02x} + je -> TYPE 0x{cmp_val:02x}")

                    elif ni.mnemonic in ('jg', 'ja'):
                        # Binary split: upper half goes to target
                        target = get_jump_target(ni)
                        # Check if je follows (exact match)
                        if i + 2 < len(insns) and insns[i + 2].mnemonic == 'je':
                            types.add(cmp_val)
                            if verbose:
                                print(f"    {'  '*depth}[{label}] cmp 0x{cmp_val:02x} + jg + je -> TYPE 0x{cmp_val:02x}, follow jg @ 0x{target:04x}")
                        else:
                            if verbose:
                                print(f"    {'  '*depth}[{label}] cmp 0x{cmp_val:02x} + jg (no je) -> follow jg @ 0x{target:04x}")
                        if target:
                            walk(target, 0, depth + 1, f"jg(>{cmp_val:#x})")

                    elif ni.mnemonic in ('jge', 'jae'):
                        target = get_jump_target(ni)
                        if i + 2 < len(insns) and insns[i + 2].mnemonic == 'je':
                            types.add(cmp_val)
                        if target:
                            walk(target, 0, depth + 1, f"jge(>={cmp_val:#x})")

                    elif ni.mnemonic in ('jle', 'jbe'):
                        # Binary split: lower half (including equal) goes to target
                        target = get_jump_target(ni)
                        # jle includes equal case, so cmp_val might be handled in lower branch
                        # Don't add cmp_val as type unless there's a separate je
                        if verbose:
                            print(f"    {'  '*depth}[{label}] cmp 0x{cmp_val:02x} + jle -> follow jle @ 0x{target:04x}")
                        if target:
                            walk(target, 0, depth + 1, f"jle(<={cmp_val:#x})")

                    elif ni.mnemonic in ('jl', 'jb'):
                        target = get_jump_target(ni)
                        # jl does NOT include equal case
                        if i + 2 < len(insns) and insns[i + 2].mnemonic == 'je':
                            types.add(cmp_val)
                        if target:
                            walk(target, 0, depth + 1, f"jl(<{cmp_val:#x})")

                    elif ni.mnemonic == 'jne':
                        # Fall-through is handler for cmp_val
                        types.add(cmp_val)
                        target = get_jump_target(ni)
                        if verbose:
                            print(f"    {'  '*depth}[{label}] cmp 0x{cmp_val:02x} + jne -> TYPE 0x{cmp_val:02x}, follow jne @ 0x{target:04x}" if target else
                                  f"    {'  '*depth}[{label}] cmp 0x{cmp_val:02x} + jne -> TYPE 0x{cmp_val:02x} (last)")
                        if target:
                            walk(target, acc, depth + 1, f"jne(cmp {cmp_val:#x})")
                        return  # Fall-through is handler code

            # Handle test reg, reg (check for zero)
            elif insn.mnemonic == 'test' and len(insn.operands) == 2:
                op1, op2 = insn.operands
                if op1.type == CS_OP_REG and op2.type == CS_OP_REG and op1.reg == op2.reg:
                    # test eax, eax — checking if accumulated value is zero
                    if i + 1 < len(insns):
                        ni = insns[i + 1]
                        if ni.mnemonic == 'je':
                            types.add(acc)
                            if verbose:
                                print(f"    {'  '*depth}[{label}] test+je -> TYPE 0x{acc:02x}")

            # Handle sub
            elif insn.mnemonic == 'sub' and len(insn.operands) == 2:
                if insn.operands[0].type == CS_OP_REG and insn.operands[1].type == CS_OP_IMM:
                    delta = insn.operands[1].imm & 0xFFFFFFFF
                    if delta <= 0xFF:
                        acc += delta
                        if i + 1 < len(insns):
                            ni = insns[i + 1]
                            if ni.mnemonic == 'je':
                                types.add(acc)
                                if verbose:
                                    print(f"    {'  '*depth}[{label}] sub 0x{delta:02x} + je -> TYPE 0x{acc:02x}")
                            elif ni.mnemonic == 'jne':
                                types.add(acc)
                                target = get_jump_target(ni)
                                if verbose:
                                    print(f"    {'  '*depth}[{label}] sub 0x{delta:02x} + jne -> TYPE 0x{acc:02x}, follow jne @ 0x{target:04x}" if target else
                                          f"    {'  '*depth}[{label}] sub 0x{delta:02x} + jne -> TYPE 0x{acc:02x} (last)")
                                if target:
                                    walk(target, acc, depth + 1, f"jne(sub {acc:#x})")
                                return  # Fall-through is handler code

            # Handle dec
            elif insn.mnemonic == 'dec' and len(insn.operands) == 1:
                if insn.operands[0].type == CS_OP_REG:
                    acc += 1
                    if i + 1 < len(insns):
                        ni = insns[i + 1]
                        if ni.mnemonic == 'je':
                            types.add(acc)
                            if verbose:
                                print(f"    {'  '*depth}[{label}] dec + je -> TYPE 0x{acc:02x}")
                        elif ni.mnemonic == 'jne':
                            types.add(acc)
                            target = get_jump_target(ni)
                            if verbose:
                                print(f"    {'  '*depth}[{label}] dec + jne -> TYPE 0x{acc:02x}, follow jne @ 0x{target:04x}" if target else
                                      f"    {'  '*depth}[{label}] dec + jne -> TYPE 0x{acc:02x} (last)")
                            if target:
                                walk(target, acc, depth + 1, f"jne(dec {acc:#x})")
                            return  # Fall-through is handler code

            # Handle unconditional jmp (follow it)
            elif insn.mnemonic == 'jmp':
                target = get_jump_target(insn)
                if target:
                    walk(target, acc, depth, label + "->jmp")
                return

            # Stop conditions: we've entered a handler, not more dispatch
            elif insn.mnemonic in ('push', 'call', 'ret', 'leave', 'int3'):
                return
            elif insn.mnemonic == 'lea' and len(insn.operands) == 2:
                # lea setting up args = entering a handler
                dst = insn.operands[0]
                src = insn.operands[1]
                if dst.type == CS_OP_REG and src.type == CS_OP_MEM:
                    # lea eax, [ebp-X] or lea eax, [esp+X] = stack arg setup
                    if src.mem.base in (X86_REG_EBP, X86_REG_ESP):
                        return

            i += 1

    walk(start_offset, 0, 0, "root")
    return types


def find_movzx_after_xor(insns, xor_off):
    """Find the first movzx r32, r8 after the XOR instruction."""
    found_xor = False
    for i, insn in enumerate(insns):
        if insn.address == xor_off:
            found_xor = True
            continue
        if found_xor and insn.mnemonic == 'movzx' and len(insn.operands) == 2:
            dst, src = insn.operands
            if dst.type == CS_OP_REG and src.type == CS_OP_REG and src.size == 1:
                return i, insn
            if dst.type == CS_OP_REG and src.type == CS_OP_REG:
                # movzx eax, al — check sizes
                return i, insn
        if found_xor and i > 8:  # Don't look too far
            break
    return None, None


def analyze_xor_site(data, xor_off, verbose=False):
    """Analyze a single XOR site, return (kind, types) where kind is 'remap' or 'dispatch'."""
    insns = disasm_at(data, xor_off, 120)
    if not insns:
        return 'error', set(), None

    # Find movzx after XOR
    movzx_idx, movzx_insn = find_movzx_after_xor(insns, xor_off)
    if movzx_idx is None:
        return 'no_movzx', set(), None

    # Check if it's a remap table
    remap_info = detect_remap_table(insns, movzx_idx)
    if remap_info:
        return 'remap', set(), remap_info

    # Pre-scan instructions from XOR to movzx for register values
    # This catches patterns like: mov ecx, 0x8E; ...; movzx eax, al; cmp eax, ecx
    pre_reg_vals = {}
    for j in range(0, movzx_idx):
        pinsn = insns[j]
        if pinsn.mnemonic == 'mov' and len(pinsn.operands) == 2:
            dst, src = pinsn.operands
            if dst.type == CS_OP_REG and src.type == CS_OP_IMM:
                val = src.imm & 0xFFFFFFFF
                if val <= 0xFF:
                    pre_reg_vals[dst.reg] = val

    if verbose and pre_reg_vals:
        regs32 = {X86_REG_EAX: 'eax', X86_REG_ECX: 'ecx', X86_REG_EDX: 'edx',
                  X86_REG_EBX: 'ebx', X86_REG_ESP: 'esp', X86_REG_EBP: 'ebp',
                  X86_REG_ESI: 'esi', X86_REG_EDI: 'edi'}
        for reg, val in pre_reg_vals.items():
            rname = regs32.get(reg, f"reg{reg}")
            print(f"    pre-scan: {rname} = 0x{val:02x}")

    # It's a dispatch chain — extract types recursively
    # Start walking from the instruction AFTER movzx
    start_off = insns[movzx_idx + 1].address if movzx_idx + 1 < len(insns) else None
    if start_off is None:
        return 'error', set(), None

    types = extract_types_recursive(data, start_off, initial_reg_vals=pre_reg_vals, verbose=verbose)
    return 'dispatch', types, None


def analyze_module(hash_str, data, short_hash, verbose=False):
    """Full analysis of one module."""
    known = KNOWN_TYPES.get(short_hash, {})

    xor_hits = raw_scan_xor_byte_reg_disp(data, disp_val=4)
    print(f"  Size: {len(data)} bytes, XOR scan: {len(xor_hits)} hit(s)")

    best_dispatch = set()
    remap_count = 0
    remap_infos = []  # (off, remap_off, jtable_off, shift, max_type)
    dispatch_results = []

    for off, dst_r8, src_base in xor_hits:
        kind, types, remap_info = analyze_xor_site(data, off, verbose=verbose)

        if kind == 'remap':
            remap_count += 1
            roff, joff, shift, mtype = remap_info
            remap_infos.append((off, roff, joff, shift, mtype))
            print(f"    XOR @ 0x{off:04x}: xor {dst_r8}, [{src_base}+4] -> REMAP TABLE "
                  f"(remap=0x{roff:04x}, jtable=0x{joff:04x}, shift=0x{shift:02x}, max=0x{mtype:02x})")
        elif kind == 'dispatch':
            dispatch_results.append((off, types))
            ann = ""
            for t in sorted(types):
                if t in known:
                    ann += f" [{known[t]}]"
            print(f"    XOR @ 0x{off:04x}: xor {dst_r8}, [{src_base}+4] -> DISPATCH ({len(types)} types): "
                  f"{[f'0x{t:02x}' for t in sorted(types)]}{ann}")
            if len(types) > len(best_dispatch):
                best_dispatch = types
        else:
            print(f"    XOR @ 0x{off:04x}: xor {dst_r8}, [{src_base}+4] -> {kind}")

    # If no dispatch chain found, try extracting types from remap tables
    # Strategy: cross-reference singletons+pairs across all remap tables
    if not best_dispatch and remap_infos:
        print(f"\n  No dispatch chain found, extracting from remap table(s)...")
        sp_sets = []  # singletons+pairs per table
        for xor_off, roff, joff, shift, mtype in remap_infos:
            singletons, pairs, all_nd = get_remap_singletons_pairs(
                data, roff, shift, mtype, verbose=verbose)
            sp = set(singletons)
            for pair in pairs:
                sp.update(pair)
            sp_sets.append(sp)
            print(f"    Remap @ 0x{roff:04x} (shift=0x{shift:02x}): "
                  f"{len(singletons)} singletons + {len(pairs)} pairs = {len(sp)} candidates")

        if len(sp_sets) >= 2:
            # Cross-reference: types that are singletons or pairs in ALL tables
            intersection = sp_sets[0]
            for s in sp_sets[1:]:
                intersection = intersection & s
            best_dispatch = intersection
            print(f"    Cross-reference ({len(sp_sets)} tables): {len(best_dispatch)} types")
        elif len(sp_sets) == 1:
            # Single table: use singletons only (more conservative)
            best_dispatch = sp_sets[0]
            print(f"    Single table: {len(best_dispatch)} candidates (singletons+pairs)")

    # Report results
    if best_dispatch:
        print(f"\n  RESULT: {len(best_dispatch)} types: {[f'0x{t:02x}' for t in sorted(best_dispatch)]}")

        if known:
            found = set(known.keys()) & best_dispatch
            missing = set(known.keys()) - best_dispatch
            extra = best_dispatch - set(known.keys())
            print(f"  VALIDATION ({len(found)}/{len(known)} known types found):")
            if found:
                print(f"    Found:   {[f'0x{t:02x}={known[t]}' for t in sorted(found)]}")
            if missing:
                print(f"    MISSING: {[f'0x{t:02x}={known[t]}' for t in sorted(missing)]}")
            if extra:
                print(f"    EXTRA:   {[f'0x{t:02x}' for t in sorted(extra)]}")
            if not missing:
                print(f"    *** PERFECT MATCH ***")
    else:
        print(f"\n  WARNING: No types found!")

    return best_dispatch


def main():
    hashes = set()
    if os.path.exists(DOCS_DIR):
        for f in os.listdir(DOCS_DIR):
            if f.startswith('warden_') and '_decompressed' in f:
                parts = f.split('_')
                if len(parts) >= 3 and len(parts[1]) == 32:
                    hashes.add(parts[1])

    print(f"Analyzing {len(hashes)} modules\n")

    total = 0
    success = 0
    fail_modules = []

    for h in sorted(hashes):
        short = h[:8]
        print(f"\n{'=' * 70}")
        print(f"Module: {short}")
        print(f"{'=' * 70}")

        data = load_module(h)
        if data is None:
            print("  SKIP")
            continue

        total += 1
        types = analyze_module(h, data, short, verbose=True)
        if types and len(types) >= 3:
            success += 1
        else:
            fail_modules.append(short)

    print(f"\n\n{'=' * 70}")
    print(f"SUMMARY: {success}/{total} modules extracted successfully")
    if fail_modules:
        print(f"FAILED: {fail_modules}")
    print(f"{'=' * 70}")


if __name__ == '__main__':
    main()
