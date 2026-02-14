"""Investigate 0AC0C559 and E348326F - both have 0 dispatch chains."""

import struct, zlib, os
from capstone import *
from capstone.x86 import *

DOCS_DIR = r"C:\Users\bpqvg\source\repos\wotlk-utils\wotlk\docs\warden_dumps"

md = Cs(CS_ARCH_X86, CS_MODE_32)
md.detail = True


def load_module(hash_str):
    path = os.path.join(DOCS_DIR, f"warden_{hash_str}_decompressed.bin")
    if os.path.exists(path):
        with open(path, 'rb') as f:
            return f.read()
    return None


def disasm_at(data, offset, max_insns=120):
    end = min(offset + max_insns * 15, len(data))
    code = data[offset:end]
    return list(md.disasm(code, offset))


def show_xor_context(data, xor_off, label, context=60):
    """Show disassembly around XOR site."""
    # Disassemble from a bit before XOR
    start = max(0, xor_off - 32)
    insns = disasm_at(data, start, context)
    if not insns:
        return

    print(f"\n  === XOR @ 0x{xor_off:04x} ({label}) ===")
    for insn in insns:
        marker = ""
        if insn.address == xor_off:
            marker = " <<<< XOR"
        elif insn.mnemonic in ('movzx',):
            marker = " <<<< MOVZX"
        elif insn.mnemonic in ('cmp', 'test'):
            marker = " <<<< CMP"
        elif insn.mnemonic in ('sub', 'dec'):
            marker = " <<<< SUB/DEC"
        elif insn.mnemonic in ('je', 'jne', 'jg', 'jge', 'jl', 'jle', 'ja', 'jae', 'jb', 'jbe'):
            marker = " <<<< BRANCH"
        elif insn.mnemonic == 'jmp':
            marker = " <<<< JMP"
        raw = ' '.join(f'{b:02x}' for b in insn.bytes)
        print(f"    0x{insn.address:04x}: {raw:30s} {insn.mnemonic:8s} {insn.op_str}{marker}")


def scan_all_xor_patterns(data):
    """Find ALL xor r8, [reg+disp8] patterns for any disp8 value."""
    results = []
    for off in range(len(data) - 2):
        if data[off] != 0x32:
            continue
        modrm = data[off + 1]
        if not (0x40 <= modrm <= 0x7F):
            continue
        if (modrm & 7) == 4:  # SIB
            continue
        disp = data[off + 2]
        regs8 = ['al', 'cl', 'dl', 'bl', 'ah', 'ch', 'dh', 'bh']
        regs32 = ['eax', 'ecx', 'edx', 'ebx', 'esp', 'ebp', 'esi', 'edi']
        dst_r8 = regs8[(modrm >> 3) & 7]
        src_base = regs32[modrm & 7]
        results.append((off, dst_r8, src_base, disp))
    return results


def scan_xor_reg_reg(data):
    """Find all xor r8, r8 (register-register) patterns."""
    results = []
    for off in range(len(data) - 1):
        if data[off] != 0x32:
            continue
        modrm = data[off + 1]
        # mod=11 (register-register) -> 0xC0-0xFF
        if not (0xC0 <= modrm <= 0xFF):
            continue
        regs8 = ['al', 'cl', 'dl', 'bl', 'ah', 'ch', 'dh', 'bh']
        dst_r8 = regs8[(modrm >> 3) & 7]
        src_r8 = regs8[modrm & 7]
        # Skip self-XOR (zeroing pattern)
        if dst_r8 == src_r8:
            continue
        results.append((off, dst_r8, src_r8))
    return results


def scan_xor_mem_any_disp(data):
    """Find xor r8, [reg+disp32] patterns (mod=10)."""
    results = []
    for off in range(len(data) - 5):
        if data[off] != 0x32:
            continue
        modrm = data[off + 1]
        # mod=10 (disp32) -> 0x80-0xBF
        if not (0x80 <= modrm <= 0xBF):
            continue
        if (modrm & 7) == 4:  # SIB
            continue
        disp = struct.unpack_from('<I', data, off + 2)[0]
        if disp > 0xFFFF:
            continue  # too large to be meaningful
        regs8 = ['al', 'cl', 'dl', 'bl', 'ah', 'ch', 'dh', 'bh']
        regs32 = ['eax', 'ecx', 'edx', 'ebx', 'esp', 'ebp', 'esi', 'edi']
        dst_r8 = regs8[(modrm >> 3) & 7]
        src_base = regs32[modrm & 7]
        results.append((off, dst_r8, src_base, disp))
    return results


def investigate(hash_str, label):
    print(f"\n{'='*70}")
    print(f"Module: {label} ({hash_str[:8]})")
    print(f"{'='*70}")

    data = load_module(hash_str)
    if not data:
        print("  NOT FOUND")
        return

    print(f"  Size: {len(data)} bytes")

    # Show XOR [reg+4] sites
    xor4 = scan_all_xor_patterns(data)
    xor4_disp4 = [(o, d, s, dp) for o, d, s, dp in xor4 if dp == 4]
    print(f"\n  xor r8, [reg+4]: {len(xor4_disp4)} hit(s)")
    for off, dst, src, disp in xor4_disp4:
        show_xor_context(data, off, f"xor {dst}, [{src}+{disp}]")

    # Show ALL xor r8, [reg+disp8] with various displacements
    other_disps = set()
    for o, d, s, dp in xor4:
        if dp != 4:
            other_disps.add(dp)
    if other_disps:
        print(f"\n  xor r8, [reg+disp8] with OTHER displacements: {sorted(other_disps)}")
        for dp in sorted(other_disps):
            hits = [(o, d, s) for o, d, s, dv in xor4 if dv == dp]
            if len(hits) <= 5:
                for off, dst, src in hits:
                    print(f"    disp={dp}: xor {dst}, [{src}+{dp}] @ 0x{off:04x}")

    # Show xor r8, [reg+disp32]
    xor32 = scan_xor_mem_any_disp(data)
    if xor32:
        print(f"\n  xor r8, [reg+disp32]: {len(xor32)} hit(s)")
        for off, dst, src, disp in xor32[:10]:
            print(f"    @ 0x{off:04x}: xor {dst}, [{src}+0x{disp:04x}]")

    # Show xor r8, r8 (register-register)
    xor_rr = scan_xor_reg_reg(data)
    print(f"\n  xor r8, r8 (non-zero): {len(xor_rr)} hit(s)")
    for off, dst, src in xor_rr:
        # Check if followed by movzx within 10 instructions
        insns = disasm_at(data, off, 15)
        has_movzx = False
        has_dispatch = False
        for j, insn in enumerate(insns):
            if insn.mnemonic == 'movzx' and insn.address > off:
                has_movzx = True
            if insn.mnemonic in ('cmp', 'sub', 'dec') and insn.address > off and has_movzx:
                has_dispatch = True
                break

        marker = ""
        if has_dispatch:
            marker = " *** POTENTIAL DISPATCH ***"
        elif has_movzx:
            marker = " (has movzx)"
        print(f"    @ 0x{off:04x}: xor {dst}, {src}{marker}")

        if has_dispatch:
            show_xor_context(data, off, f"xor {dst}, {src}", context=80)


if __name__ == '__main__':
    investigate("0AC0C5599EB0FA9FC3392907F3A2918C", "0AC0C559")
    investigate("E348326FA04D9CD237FCDA4B473D97B0", "E348326F")
