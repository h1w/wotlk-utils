"""
Analyze Warden module decompressed binary to find the check type dispatcher.
Uses capstone for x86 disassembly.

Known check type IDs for module 7C4ABC97:
  0x1F=TIMING, 0x22=PAGE_A, 0x47=PAGE_B, 0x69=PROC,
  0x8E=MEM, 0x91=MPQ, 0xB3=MODULE, 0xD8=DRIVER, 0xDB=LUA
"""

import sys
import struct
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

def find_known_types_in_code(data, header, known_types=None):
    """
    Disassemble the module and find all instructions that reference
    known check type values. This helps locate the dispatcher.
    """
    md = Cs(CS_ARCH_X86, CS_MODE_32)
    md.detail = True

    start = header['packedDataOff']
    code = data[start:]

    # Phase 1: Find ALL cmp/sub/test instructions with immediate operands
    # that match known type IDs
    print(f"\n=== Disassembling from offset 0x{start:04x} ({len(code)} bytes) ===\n")

    type_refs = []  # (offset, mnemonic, op_str, imm_value)
    all_cmp_imm = []  # all cmp with byte-range immediates

    for insn in md.disasm(code, start):
        mnemonic = insn.mnemonic

        # Look for cmp, sub, test, and, xor, movzx with immediates
        if mnemonic in ('cmp', 'sub', 'test', 'and', 'xor', 'add'):
            # Check if second operand is an immediate in byte range
            if insn.op_count(CS_OP_IMM) > 0:
                for op in insn.operands:
                    if op.type == CS_OP_IMM:
                        imm = op.imm & 0xFF
                        if 0 < imm < 0xFF and op.imm == imm:  # truly byte-range
                            all_cmp_imm.append((insn.address, mnemonic, insn.op_str, imm))
                            if known_types and imm in known_types:
                                type_refs.append((insn.address, mnemonic, insn.op_str, imm))

    if known_types:
        print(f"=== Instructions referencing known type IDs ({len(type_refs)} found) ===")
        for addr, mn, ops, imm in type_refs:
            print(f"  0x{addr:04x}: {mn:6s} {ops:30s}  ; imm=0x{imm:02x}")

        # Group by proximity - find clusters
        if type_refs:
            print(f"\n=== Clustering by proximity (window=256 bytes) ===")
            clusters = []
            current = [type_refs[0]]
            for ref in type_refs[1:]:
                if ref[0] - current[-1][0] < 256:
                    current.append(ref)
                else:
                    clusters.append(current)
                    current = [ref]
            clusters.append(current)

            for i, cluster in enumerate(clusters):
                unique_imms = set(r[3] for r in cluster)
                span = cluster[-1][0] - cluster[0][0]
                print(f"\n  Cluster #{i+1}: {len(cluster)} refs, {len(unique_imms)} unique types, "
                      f"span={span} bytes (0x{cluster[0][0]:04x}..0x{cluster[-1][0]:04x})")
                for addr, mn, ops, imm in cluster:
                    print(f"    0x{addr:04x}: {mn:6s} {ops:30s}  ; 0x{imm:02x}")
                print(f"    Unique type IDs: {sorted([f'0x{x:02x}' for x in unique_imms])}")

    return all_cmp_imm, type_refs

def disasm_around(data, header, center_offset, window=128):
    """Disassemble a window around a given offset for context."""
    md = Cs(CS_ARCH_X86, CS_MODE_32)
    md.detail = True

    start = max(header['packedDataOff'], center_offset - window)
    end = min(len(data), center_offset + window)
    code = data[start:end]

    print(f"\n=== Disassembly around 0x{center_offset:04x} (0x{start:04x}..0x{end:04x}) ===")
    for insn in md.disasm(code, start):
        marker = " <<<<" if insn.address == center_offset else ""
        # Show raw bytes
        raw = ' '.join(f'{b:02x}' for b in insn.bytes)
        print(f"  0x{insn.address:04x}: {raw:24s} {insn.mnemonic:8s} {insn.op_str}{marker}")

def find_dispatcher_pattern(data, header):
    """
    Look for the actual dispatch pattern by finding:
    1. Function that reads a byte (check type) and branches on it
    2. Switch-like patterns: jump tables, indirect jumps, cmp chains
    """
    md = Cs(CS_ARCH_X86, CS_MODE_32)
    md.detail = True

    start = header['packedDataOff']
    code = data[start:]

    # Look for indirect jumps (jmp reg, jmp [reg+reg*4+...]) - jump table dispatch
    print(f"\n=== Indirect jumps/calls (potential jump table dispatch) ===")
    indirect_jumps = []
    for insn in md.disasm(code, start):
        if insn.mnemonic in ('jmp', 'call'):
            # Check if operand is memory or register (not immediate)
            if insn.op_count(CS_OP_IMM) == 0 and len(insn.operands) > 0:
                op = insn.operands[0]
                if op.type == CS_OP_MEM:
                    # Memory operand - could be jump table [base + idx*4]
                    mem = op.mem
                    if mem.scale == 4:  # scaled index = jump table
                        indirect_jumps.append((insn.address, insn.mnemonic, insn.op_str, 'JUMP_TABLE'))
                        print(f"  0x{insn.address:04x}: {insn.mnemonic} {insn.op_str}  *** JUMP TABLE ***")
                    elif mem.scale in (0, 1) and mem.index == 0:
                        # Simple memory dereference
                        pass
                    else:
                        indirect_jumps.append((insn.address, insn.mnemonic, insn.op_str, 'indirect'))
                        print(f"  0x{insn.address:04x}: {insn.mnemonic} {insn.op_str}")
                elif op.type == CS_OP_REG:
                    indirect_jumps.append((insn.address, insn.mnemonic, insn.op_str, 'reg'))
                    print(f"  0x{insn.address:04x}: {insn.mnemonic} {insn.op_str}")

    # Look for movzx (byte to dword) followed by comparison or table lookup
    print(f"\n=== movzx patterns (byte load + potential dispatch) ===")
    insns = list(md.disasm(code, start))
    for i, insn in enumerate(insns):
        if insn.mnemonic == 'movzx':
            # Check next few instructions for cmp, sub, jmp, or table access
            context = insns[i:i+8]
            has_dispatch = False
            for next_insn in context[1:]:
                if next_insn.mnemonic in ('jmp', 'call') and next_insn.op_count(CS_OP_IMM) == 0:
                    has_dispatch = True
                if next_insn.mnemonic in ('cmp', 'sub') and next_insn.op_count(CS_OP_IMM) > 0:
                    has_dispatch = True
            if has_dispatch:
                print(f"\n  movzx at 0x{insn.address:04x}: {insn.op_str}")
                for ci in context:
                    raw = ' '.join(f'{b:02x}' for b in ci.bytes)
                    print(f"    0x{ci.address:04x}: {raw:24s} {ci.mnemonic:8s} {ci.op_str}")

    return indirect_jumps

def analyze_module(path, known_types=None):
    print(f"\n{'='*60}")
    print(f"Analyzing: {path}")
    print(f"{'='*60}")

    data = read_module(path)
    header = parse_header(data)
    if not header:
        print("ERROR: Invalid header")
        return

    print(f"Module size: {len(data)} bytes")
    print(f"Runtime size: {header['moduleSize']}")
    print(f"Sections: {header['sectionDescCount']}")
    print(f"Packed data at: 0x{header['packedDataOff']:04x}")
    print(f"Reloc count: {header['relocCount']}")
    print(f"Import libs: {header['importLibCount']}")

    # Phase 1: Find references to known types
    all_cmp, type_refs = find_known_types_in_code(data, header, known_types)

    # Phase 2: Find indirect jump patterns
    indirect = find_dispatcher_pattern(data, header)

    # Phase 3: If we found clusters of known type refs, disasm around them
    if type_refs:
        # Find the densest cluster
        best_cluster = []
        for i in range(len(type_refs)):
            window = [r for r in type_refs if abs(r[0] - type_refs[i][0]) < 256]
            if len(window) > len(best_cluster):
                best_cluster = window

        if best_cluster:
            center = best_cluster[len(best_cluster)//2][0]
            disasm_around(data, header, center, window=200)

    # Phase 4: For jump tables, show context
    for addr, mn, ops, kind in indirect:
        if kind == 'JUMP_TABLE':
            disasm_around(data, header, addr, window=100)

if __name__ == '__main__':
    # Module 7C4ABC97 - known types
    known_7c = {0x1F, 0x22, 0x47, 0x69, 0x8E, 0x91, 0xB3, 0xD8, 0xDB}

    base = r"C:\Users\bpqvg\source\repos\wotlk-utils\wotlk\docs\warden_dumps"

    analyze_module(
        f"{base}\\warden_7C4ABC97B86494A2D91820785F3A1C87_decompressed.bin",
        known_types=known_7c
    )
