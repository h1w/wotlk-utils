"""
Deep analysis of module 9A95D199 to find the dispatcher.
More flexible patterns - no assumptions about register usage or offsets.
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

def analyze(path, label):
    print(f"\n{'='*70}")
    print(f"Module: {label}")
    print(f"{'='*70}")

    data = read_module(path)
    header = parse_header(data)
    start = header['packedDataOff']
    code = data[start:]

    md = Cs(CS_ARCH_X86, CS_MODE_32)
    md.detail = True
    insns = list(md.disasm(code, start))
    print(f"Disassembled {len(insns)} instructions from offset 0x{start:04x}")

    # 1. Find ALL jmp [reg*4 + disp] (jump tables)
    print(f"\n--- All jmp [reg*4 + disp] (jump tables) ---")
    for insn in insns:
        if insn.mnemonic == 'jmp' and len(insn.operands) == 1:
            op = insn.operands[0]
            if op.type == CS_OP_MEM and op.mem.scale == 4:
                print(f"  0x{insn.address:04x}: {insn.mnemonic} {insn.op_str}")

    # 2. Find ALL movzx byte + nearby jmp (any form)
    print(f"\n--- movzx byte -> remap table lookups ---")
    for i, insn in enumerate(insns):
        if insn.mnemonic == 'movzx' and len(insn.operands) == 2:
            src = insn.operands[1]
            if src.type == CS_OP_MEM and src.size == 1 and src.mem.disp > 0x40:
                # Memory byte load with significant displacement
                for j in range(i+1, min(i+5, len(insns))):
                    ni = insns[j]
                    if ni.mnemonic == 'jmp' and len(ni.operands) == 1:
                        nop = ni.operands[0]
                        if nop.type == CS_OP_MEM and nop.mem.scale in (2, 4):
                            print(f"  REMAP+JMP at 0x{insn.address:04x}:")
                            for k in range(max(0,i-3), min(len(insns), j+3)):
                                marker = " <<<" if insns[k].address in (insn.address, ni.address) else ""
                                raw = ' '.join(f'{b:02x}' for b in insns[k].bytes)
                                print(f"    0x{insns[k].address:04x}: {raw:24s} {insns[k].mnemonic:8s} {insns[k].op_str}{marker}")

    # 3. Find ALL xor with single-byte, followed by movzx + branching
    print(f"\n--- XOR patterns (type byte unmasking) ---")
    for i, insn in enumerate(insns):
        if insn.mnemonic == 'xor' and len(insn.operands) == 2:
            src = insn.operands[1]
            dst = insn.operands[0]
            # xor reg8, mem8 (XOR al/cl/dl, byte ptr [reg + offset])
            if dst.type == CS_OP_REG and src.type == CS_OP_MEM and src.size == 1:
                # Look for movzx shortly after
                has_movzx = False
                has_cmp = False
                for j in range(i+1, min(i+6, len(insns))):
                    if insns[j].mnemonic == 'movzx':
                        has_movzx = True
                    if insns[j].mnemonic in ('cmp', 'sub', 'test'):
                        has_cmp = True
                if has_movzx and has_cmp:
                    print(f"\n  XOR at 0x{insn.address:04x}: {insn.mnemonic} {insn.op_str}")
                    for k in range(i, min(i+15, len(insns))):
                        raw = ' '.join(f'{b:02x}' for b in insns[k].bytes)
                        print(f"    0x{insns[k].address:04x}: {raw:24s} {insns[k].mnemonic:8s} {insns[k].op_str}")

    # 4. Find ALL switch-like patterns: cmp reg, imm + ja/jb/je chains
    print(f"\n--- cmp + ja (range check before dispatch) ---")
    for i, insn in enumerate(insns):
        if insn.mnemonic == 'cmp' and len(insn.operands) == 2:
            imm_op = None
            for op in insn.operands:
                if op.type == CS_OP_IMM:
                    imm_val = op.imm & 0xFFFFFFFF
                    if 0x80 <= imm_val <= 0xFF:
                        imm_op = imm_val
            if imm_op is None:
                continue
            # Check if next instruction is ja/jb/jbe/jae
            if i + 1 < len(insns):
                ni = insns[i+1]
                if ni.mnemonic in ('ja', 'jb', 'jae', 'jbe', 'jg', 'jge'):
                    print(f"\n  Range check at 0x{insn.address:04x}:")
                    for k in range(max(0,i-3), min(len(insns), i+8)):
                        raw = ' '.join(f'{b:02x}' for b in insns[k].bytes)
                        print(f"    0x{insns[k].address:04x}: {raw:24s} {insns[k].mnemonic:8s} {insns[k].op_str}")

    # 5. Broader search: any movzx eax/ecx/edx, byte + indirect jmp within 10 insns
    print(f"\n--- Any movzx byte -> indirect jmp (within 10 insns) ---")
    for i, insn in enumerate(insns):
        if insn.mnemonic == 'movzx' and len(insn.operands) == 2:
            src = insn.operands[1]
            if src.size == 1:  # byte source
                for j in range(i+1, min(i+10, len(insns))):
                    ni = insns[j]
                    if ni.mnemonic == 'jmp' and len(ni.operands) == 1:
                        nop = ni.operands[0]
                        if nop.type == CS_OP_MEM and nop.mem.scale >= 2:
                            print(f"\n  movzx at 0x{insn.address:04x} -> jmp at 0x{ni.address:04x}:")
                            for k in range(max(0,i-2), min(len(insns), j+3)):
                                raw = ' '.join(f'{b:02x}' for b in insns[k].bytes)
                                print(f"    0x{insns[k].address:04x}: {raw:24s} {insns[k].mnemonic:8s} {insns[k].op_str}")
                            break  # only first match per movzx

    # 6. Raw byte scan for jump table pattern: FF 24 85/8D/95/BD xx xx xx xx
    print(f"\n--- Raw byte scan: jmp dword ptr [reg*4 + disp32] ---")
    # FF 24 85 = jmp [eax*4 + disp32]
    # FF 24 8D = jmp [ecx*4 + disp32]
    # FF 24 95 = jmp [edx*4 + disp32]
    for off in range(len(data) - 7):
        if data[off] == 0xFF and data[off+1] == 0x24:
            modrm = data[off+2]
            if modrm in (0x85, 0x8D, 0x95, 0x9D, 0xA5, 0xAD, 0xB5, 0xBD):
                disp = struct.unpack_from('<I', data, off+3)[0]
                reg_names = {0x85:'eax',0x8D:'ecx',0x95:'edx',0x9D:'ebx',
                             0xA5:'esp',0xAD:'ebp',0xB5:'esi',0xBD:'edi'}
                reg = reg_names.get(modrm, '???')
                print(f"  offset 0x{off:04x}: FF 24 {modrm:02x} -> jmp [{reg}*4 + 0x{disp:08x}]")

    # 7. Raw byte scan for remap table: 0F B6 80/81/82/83 xx xx xx xx (movzx eax, byte ptr [eax+disp32])
    print(f"\n--- Raw byte scan: movzx r32, byte ptr [r32 + disp32] ---")
    for off in range(len(data) - 7):
        if data[off] == 0x0F and data[off+1] == 0xB6:
            modrm = data[off+2]
            # ModRM: mod=10 (disp32), reg=0-7, rm=0-7 → 0x80-0xBF
            if 0x80 <= modrm <= 0xBF and modrm not in (0x84, 0x8C, 0x94, 0x9C, 0xA4, 0xAC, 0xB4, 0xBC):
                # Not SIB-based (rm != 4)
                disp = struct.unpack_from('<I', data, off+3)[0]
                if disp > 0x100 and disp < len(data):  # reasonable remap table offset
                    reg_dst = (modrm >> 3) & 7
                    reg_src = modrm & 7
                    regs = ['eax','ecx','edx','ebx','esp','ebp','esi','edi']
                    print(f"  offset 0x{off:04x}: movzx {regs[reg_dst]}, byte ptr [{regs[reg_src]} + 0x{disp:04x}]")

if __name__ == '__main__':
    base = r"C:\Users\bpqvg\source\repos\wotlk-utils\wotlk\docs\warden_dumps"

    analyze(
        f"{base}\\warden_9A95D19959AA88542116BE3639C0EB0F_decompressed.bin",
        "9A95D199"
    )
