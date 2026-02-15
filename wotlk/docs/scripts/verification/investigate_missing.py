"""
Investigate modules 85F902 and 9A95D199 that have NO xor+movzx patterns
but DO have remap tables found by raw byte scan.

Strategy:
1. Find the remap table code by raw byte scan
2. Disassemble backwards to find how the type byte is loaded and XOR'd
3. Look for the request parser (which uses actual type IDs)
"""

import struct
import zlib
import os
from capstone import *

DUMP_DIR = r"Z:\Games\wow 3.3.5a client\warden_dumps"
DOCS_DIR = r"C:\Users\bpqvg\source\repos\wotlk-utils\wotlk\docs\warden_dumps"

def load_module(hash_str):
    dirs = [DUMP_DIR, DOCS_DIR]
    for d in dirs:
        path = os.path.join(d, f"warden_{hash_str}_decompressed.bin")
        if os.path.exists(path):
            with open(path, 'rb') as f:
                return f.read()
    return None

def parse_header(data):
    fields = struct.unpack_from('<10I', data, 0)
    names = ['moduleSize', 'reserved', 'relocDataOff', 'relocCount',
             'exportTableOff', 'exportCount', 'baseIndex',
             'importTableOff', 'importLibCount', 'sectionDescCount']
    h = dict(zip(names, fields))
    h['packedDataOff'] = 0x28 + h['sectionDescCount'] * 12
    return h

def find_remap_code(data):
    """Find the movzx+jmp pattern by raw byte scan."""
    results = []
    for off in range(len(data) - 7):
        if data[off] == 0x0F and data[off+1] == 0xB6:
            modrm = data[off+2]
            if 0x80 <= modrm <= 0xBF and (modrm & 7) != 4:
                disp = struct.unpack_from('<I', data, off+3)[0]
                if 0x100 < disp < len(data):
                    # Check for jmp [reg*4 + disp] within 50 bytes
                    for j in range(off+7, min(off+57, len(data)-7)):
                        if data[j] == 0xFF and data[j+1] == 0x24:
                            sib = data[j+2]
                            if (sib >> 6) == 2 and (sib & 7) == 5:
                                jtable = struct.unpack_from('<I', data, j+3)[0]
                                results.append({
                                    'movzx_off': off,
                                    'jmp_off': j,
                                    'remap': disp,
                                    'jtable': jtable,
                                })
    return results

def disasm_window(data, start, end):
    """Disassemble a byte range and return instruction list."""
    md = Cs(CS_ARCH_X86, CS_MODE_32)
    md.detail = True
    code = data[start:end]
    return list(md.disasm(code, start))

def find_all_xor_patterns(data, scan_start):
    """Find ALL xor instructions (any form) in the module."""
    md = Cs(CS_ARCH_X86, CS_MODE_32)
    md.detail = True
    code = data[scan_start:]
    insns = list(md.disasm(code, scan_start))

    xors = []
    for i, insn in enumerate(insns):
        if insn.mnemonic == 'xor':
            xors.append((i, insn))
    return insns, xors

def investigate_module(hash_str, label):
    print(f"\n{'='*70}")
    print(f"Module: {label} ({hash_str[:8]})")
    print(f"{'='*70}")

    data = load_module(hash_str)
    if not data:
        print("  NOT FOUND")
        return
    header = parse_header(data)
    scan_start = header['packedDataOff']
    print(f"  Size: {len(data)} bytes, scan from 0x{scan_start:04x}")

    # 1. Find remap code
    remap_results = find_remap_code(data)
    print(f"\n  Remap+JMP patterns found: {len(remap_results)}")
    for r in remap_results:
        print(f"    movzx @ 0x{r['movzx_off']:04x}, jmp @ 0x{r['jmp_off']:04x}")
        print(f"    remap=0x{r['remap']:04x}, jtable=0x{r['jtable']:04x}")

        # Disassemble 200 bytes before the remap code
        window_start = max(scan_start, r['movzx_off'] - 200)
        window_end = r['jmp_off'] + 20
        insns = disasm_window(data, window_start, window_end)

        print(f"\n    Code leading to remap dispatch (from 0x{window_start:04x}):")
        for insn in insns:
            marker = ""
            if insn.address == r['movzx_off']:
                marker = " <<<< REMAP LOOKUP"
            elif insn.address == r['jmp_off']:
                marker = " <<<< JUMP TABLE"
            elif insn.mnemonic == 'xor':
                marker = " <<<< XOR"
            elif insn.mnemonic == 'cmp' or insn.mnemonic == 'sub':
                marker = " <<<< CMP/SUB"
            elif insn.mnemonic in ('ja', 'jb', 'jbe', 'jae', 'jg', 'jge', 'jl', 'jle'):
                marker = " <<<< BRANCH"
            raw = ' '.join(f'{b:02x}' for b in insn.bytes)
            print(f"    0x{insn.address:04x}: {raw:30s} {insn.mnemonic:8s} {insn.op_str}{marker}")

    # 2. Find ALL xor instructions
    all_insns, all_xors = find_all_xor_patterns(data, scan_start)
    print(f"\n  Total XOR instructions in module: {len(all_xors)}")

    # Categorize xors
    xor_mem_byte = []  # xor reg8, byte [mem] or xor byte [mem], reg8
    xor_reg_reg = []   # xor reg, reg (zeroing or data XOR)

    for idx, insn in all_xors:
        if len(insn.operands) != 2:
            continue
        op1, op2 = insn.operands

        # xor reg8, byte ptr [mem+disp]
        if op1.type == CS_OP_REG and op2.type == CS_OP_MEM and op2.size == 1:
            xor_mem_byte.append((idx, insn, 'reg8,mem8'))
        # xor byte ptr [mem+disp], reg8
        elif op1.type == CS_OP_MEM and op1.size == 1 and op2.type == CS_OP_REG:
            xor_mem_byte.append((idx, insn, 'mem8,reg8'))
        # xor reg, reg
        elif op1.type == CS_OP_REG and op2.type == CS_OP_REG:
            if op1.reg == op2.reg:
                pass  # zeroing pattern, skip
            else:
                xor_reg_reg.append((idx, insn, 'reg,reg'))
        # xor reg, imm
        elif op1.type == CS_OP_REG and op2.type == CS_OP_IMM:
            if op2.imm != 0:
                xor_reg_reg.append((idx, insn, 'reg,imm'))
        # xor reg8, reg8
        elif op1.type == CS_OP_REG and op2.type == CS_OP_REG and op1.size == 1:
            xor_reg_reg.append((idx, insn, 'reg8,reg8'))

    print(f"  XOR reg8,mem8 / mem8,reg8: {len(xor_mem_byte)}")
    for idx, insn, form in xor_mem_byte:
        # Show context
        context_start = max(0, idx - 3)
        context_end = min(len(all_insns), idx + 10)
        print(f"\n    XOR [{form}] at 0x{insn.address:04x}: {insn.mnemonic} {insn.op_str}")
        for k in range(context_start, context_end):
            ci = all_insns[k]
            marker = " <<<" if k == idx else ""
            raw = ' '.join(f'{b:02x}' for b in ci.bytes)
            print(f"      0x{ci.address:04x}: {raw:24s} {ci.mnemonic:8s} {ci.op_str}{marker}")

    print(f"\n  XOR reg,reg / reg,imm (non-zeroing): {len(xor_reg_reg)}")
    for idx, insn, form in xor_reg_reg[:10]:  # Show first 10
        print(f"    0x{insn.address:04x}: {insn.mnemonic} {insn.op_str} [{form}]")

    # 3. Look for the request parser pattern: xor followed by sub chain
    # In these modules, the request parser might NOT use 'xor reg8, byte [reg+4]'
    # It might load the xorByte into a register first, then xor
    print(f"\n  === Looking for sub chain dispatchers (cumulative sub+je) ===")
    for i in range(len(all_insns) - 20):
        insn = all_insns[i]
        # Look for: movzx eax, al (or similar) followed by sub chain
        if insn.mnemonic != 'movzx':
            continue
        if len(insn.operands) != 2:
            continue
        src = insn.operands[1]
        if src.size != 1:
            continue

        # Count sub+je pairs after this movzx
        sub_je_chain = []
        cumulative = 0
        for j in range(i+1, min(i+60, len(all_insns))):
            ni = all_insns[j]
            if ni.mnemonic in ('sub', 'dec') and len(ni.operands) >= 1:
                if ni.mnemonic == 'dec':
                    cumulative += 1
                elif ni.operands[-1].type == CS_OP_IMM:
                    cumulative += ni.operands[-1].imm & 0xFF
                # Check if next is je/jne
                if j + 1 < len(all_insns):
                    next_ni = all_insns[j+1]
                    if next_ni.mnemonic in ('je', 'jne'):
                        sub_je_chain.append(cumulative)
            elif ni.mnemonic in ('cmp', 'test'):
                if ni.operands[-1].type == CS_OP_IMM:
                    # cmp/test+je is also a type check
                    val = ni.operands[-1].imm & 0xFF
                    if j + 1 < len(all_insns) and all_insns[j+1].mnemonic in ('je', 'jne', 'jg', 'jl', 'jge', 'jle'):
                        sub_je_chain.append(val)

        if len(sub_je_chain) >= 3:
            print(f"\n    SUB/CMP chain at 0x{insn.address:04x}: {insn.mnemonic} {insn.op_str}")
            print(f"      Accumulated type values: {[f'0x{v:02x}' for v in sub_je_chain]}")
            # Show context
            for k in range(max(0, i-5), min(len(all_insns), i+25)):
                ci = all_insns[k]
                marker = " <<<" if ci.address == insn.address else ""
                raw = ' '.join(f'{b:02x}' for b in ci.bytes)
                print(f"      0x{ci.address:04x}: {raw:24s} {ci.mnemonic:8s} {ci.op_str}{marker}")

if __name__ == '__main__':
    investigate_module(
        "85F90209810FD8D63A682A35060D0AD6",
        "85F902"
    )
    investigate_module(
        "9A95D19959AA88542116BE3639C0EB0F",
        "9A95D199"
    )
