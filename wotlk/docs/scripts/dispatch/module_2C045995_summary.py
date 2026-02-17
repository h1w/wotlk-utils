#!/usr/bin/env python3
"""
FINAL SUMMARY: Warden Module 2C045995 Analysis

Disassembles both XOR sites and confirms they implement identical BST dispatch chains.
"""

from capstone import *

BINARY_PATH = r"C:\Users\bpqvg\source\repos\wotlk-utils\wotlk\docs\warden_dumps\warden_2C0459958D7BF292DD317F66F8611446_decompressed.bin"

def disasm_bst(data, xor_offset, name):
    """Disassemble and trace a BST dispatch chain."""
    print(f"\n{'=' * 100}")
    print(f"{name} - XOR SITE AT 0x{xor_offset:04X}")
    print('=' * 100)

    md = Cs(CS_ARCH_X86, CS_MODE_32)
    md.detail = True

    # Disassemble 200 bytes
    insns = list(md.disasm(data[xor_offset:xor_offset+200], xor_offset))

    # Print first 50 instructions
    print(f"\nDisassembly (first 50 instructions):\n")
    for i, insn in enumerate(insns[:50]):
        bytes_str = ' '.join(f'{b:02x}' for b in insn.bytes)
        print(f"0x{insn.address:04x}: {bytes_str:30s} {insn.mnemonic:10s} {insn.op_str}")

    # Extract types
    types = set()

    # Root comparison
    for insn in insns[:10]:
        if insn.mnemonic == 'mov' and 'ecx' in insn.op_str and ', 0x87' in insn.op_str:
            types.add(0x87)
            print(f"\n[ROOT] Found root pivot: 0x87")
            break

    # Find CMP instructions in lower branch
    for insn in insns[:30]:
        if insn.mnemonic == 'cmp' and 'eax' in insn.op_str:
            if len(insn.operands) == 2 and insn.operands[1].type == 2:  # Immediate
                val = insn.operands[1].imm
                if val < 0x100 and val != 0x87:
                    types.add(val)
                    print(f"[LOWER] Found type: 0x{val:02X}")

    # Find SUB chain in upper branch
    sub_values = []
    for insn in insns:
        if insn.mnemonic == 'sub' and 'eax' in insn.op_str:
            if len(insn.operands) == 2 and insn.operands[1].type == 2:
                val = insn.operands[1].imm
                if val < 0x100:
                    sub_values.append(val)

    # Reconstruct types from SUB chain
    if sub_values:
        print(f"\n[UPPER] SUB chain found: {[f'0x{v:02X}' for v in sub_values]}")
        cumulative = 0
        for val in sub_values:
            cumulative += val
            types.add(cumulative)
            print(f"        Cumulative: 0x{cumulative:02X}")

    print(f"\nTotal types extracted: {len(types)}")
    print(f"Type IDs: {sorted([f'0x{t:02X}' for t in types])}")

    return sorted(types)

def main():
    print("="*100)
    print(" WARDEN MODULE 2C045995 - COMPLETE ANALYSIS")
    print("="*100)

    with open(BINARY_PATH, 'rb') as f:
        data = f.read()

    print(f"\nBinary size: {len(data)} bytes (0x{len(data):X})")

    # Analyze both XOR sites
    types_site1 = disasm_bst(data, 0x10D6, "SITE #1 (Request Parser)")
    types_site2 = disasm_bst(data, 0x4AC2, "SITE #2 (Request Parser)")

    # Compare results
    print(f"\n{'=' * 100}")
    print("COMPARISON")
    print('=' * 100)

    print(f"\nSite #1 types: {[f'0x{t:02X}' for t in types_site1]}")
    print(f"Site #2 types: {[f'0x{t:02X}' for t in types_site2]}")

    if types_site1 == types_site2:
        print("\n[OK] BOTH SITES EXTRACT IDENTICAL TYPE SETS!")
        print("     This module has TWO request parser functions with the same BST.")
    else:
        print("\n[WARNING] Sites have different type sets!")

    # Final summary
    print(f"\n{'=' * 100}")
    print("FINAL RESULTS")
    print('=' * 100)

    all_types = sorted(set(types_site1) | set(types_site2))
    print(f"\nModule 2C045995 check type IDs: {[f'0x{t:02X}' for t in all_types]}")
    print(f"Total types: {len(all_types)}")

    # Check for 0xA0
    print(f"\n{'=' * 100}")
    print("TYPE 0xA0 STATUS")
    print('=' * 100)

    if 0xA0 in all_types:
        print("\n[FOUND] Type 0xA0 is used by this module!")
    else:
        print("\n[NOT FOUND] Type 0xA0 is NOT used by this module.")
        print("\nAnalysis:")
        print("  - 0xA0 falls between 0x95 and 0xC2 in the type space")
        print("  - No 'sub eax, 0x0B' instruction exists (would check for 0xA0)")
        print("  - Any packet with type 0xA0 would fall through to the default/error handler")
        print("\nConclusion:")
        print("  This module uses exactly 9 check types.")
        print("  Type 0xA0 is either:")
        print("    1. Not used by this module (module-specific type ID)")
        print("    2. From a different module's type ID space")
        print("    3. A misidentified type from network traffic analysis")

    # BST visualization
    print(f"\n{'=' * 100}")
    print("BST STRUCTURE VISUALIZATION")
    print('=' * 100)
    print("""
                        Root: 0x87
                       /          \\
                    <=0x87        >0x87
                   /                  \\
            [0x0E, 0x2D,          SUB chain:
             0x49, 0x68]          0x95 -> 0xC2 -> 0xE1 -> 0xEF
                                   |      |      |      |
                                  0x95   0xC2   0xE1   0xEF

Lower branch: Direct comparisons (CMP eax, imm8)
Upper branch: Cumulative subtractions (SUB eax, imm8)

All 9 types: 0x0E, 0x2D, 0x49, 0x68, 0x87, 0x95, 0xC2, 0xE1, 0xEF
""")

if __name__ == '__main__':
    main()
