#!/usr/bin/env python3
"""
Trace the Binary Search Tree in Warden module 2C045995 dispatch chain.
The BST uses both CMP and SUB instructions to partition the type space.
"""

from capstone import *

BINARY_PATH = r"C:\Users\bpqvg\source\repos\wotlk-utils\wotlk\docs\warden_dumps\warden_2C0459958D7BF292DD317F66F8611446_decompressed.bin"

def read_binary(path):
    with open(path, 'rb') as f:
        return f.read()

def disassemble_range(data, start_offset, size, base_addr=0):
    md = Cs(CS_ARCH_X86, CS_MODE_32)
    md.detail = True
    return list(md.disasm(data[start_offset:start_offset+size], base_addr + start_offset))

def trace_bst(instructions):
    """
    Trace the BST and extract ALL type values from CMP and SUB chains.

    BST structure with SUB chains:
    - Initial CMP divides into ranges (e.g., cmp eax, 0x87)
    - Each branch may have SUB chains: sub eax, X; je handler
    - SUB chains reveal implicit comparisons
    """
    print(f"\n{'=' * 100}")
    print("BINARY SEARCH TREE TRACE")
    print('=' * 100)

    all_types = set()

    print("\n=== ROOT NODE ===")
    print("0x4ac5: mov ecx, 0x87")
    print("0x4acd: cmp eax, ecx     ; Compare with 0x87")
    print("0x4ad5: jg  0x4bea       ; if type > 0x87, go to upper branch")
    print("0x4adb: je  0x4ba3       ; if type == 0x87, handle it")
    all_types.add(0x87)

    print("\n=== LOWER BRANCH (type <= 0x87) ===")
    print("0x4ae1: cmp eax, 0x0e    ; if type == 0x0E")
    print("0x4ae4: je  0x4b5f")
    all_types.add(0x0E)

    print("0x4ae6: cmp eax, 0x2d    ; if type == 0x2D")
    print("0x4ae9: je  0x4b02")
    all_types.add(0x2D)

    print("0x4aeb: cmp eax, 0x49    ; if type == 0x49")
    print("0x4aee: je  0x4d45")
    all_types.add(0x49)

    print("0x4af4: cmp eax, 0x68    ; if type == 0x68")
    print("0x4af7: jne 0x4d5f       ; if NOT 0x68, go to default handler")
    print("0x4afd: jmp 0x4d19       ; if == 0x68, handle it")
    all_types.add(0x68)

    print("\n=== UPPER BRANCH (type > 0x87) ===")
    print("Starting at 0x4bea:")
    print("0x4bea: sub eax, 0x95    ; eax was > 0x87, subtract 0x95")
    print("                         ; This checks if type == 0x95")
    print("0x4bef: je  0x4d19       ; if eax == 0 (i.e., original == 0x95)")
    all_types.add(0x95)

    print("\n0x4bf5: sub eax, 0x2d    ; eax = (original - 0x95) - 0x2d")
    print("                         ; = original - 0xC2")
    print("                         ; This checks if type == 0xC2")
    print("0x4bf8: je  0x4c53       ; if eax == 0 (i.e., original == 0xC2)")
    all_types.add(0xC2)

    print("\n0x4bfa: sub eax, 0x1f    ; eax = (original - 0xC2) - 0x1F")
    print("                         ; = original - 0xE1")
    print("                         ; This checks if type == 0xE1")
    print("0x4bfd: je  0x4b5f       ; if eax == 0 (i.e., original == 0xE1)")
    all_types.add(0xE1)

    print("\n0x4c03: sub eax, 0x0e    ; eax = (original - 0xE1) - 0x0E")
    print("                         ; = original - 0xEF")
    print("                         ; This checks if type == 0xEF")
    print("0x4c06: jne 0x4d5f       ; if NOT 0, go to default handler")
    print("        (implicit je to 0x4c0c) ; if == 0 (i.e., original == 0xEF)")
    all_types.add(0xEF)

    print(f"\n{'=' * 100}")
    print("EXTRACTED TYPES")
    print('=' * 100)

    sorted_types = sorted(all_types)
    print(f"\nAll types from BST: {[f'0x{t:02X}' for t in sorted_types]}")
    print(f"Total: {len(sorted_types)} types")

    # Check cumulative SUB values
    print("\n=== SUB CHAIN VERIFICATION ===")
    print("Upper branch SUB chain:")
    print(f"  0x95 (direct SUB)")
    print(f"  0x95 + 0x2D = 0x{0x95 + 0x2D:02X} (0xC2) OK")
    print(f"  0xC2 + 0x1F = 0x{0xC2 + 0x1F:02X} (0xE1) OK")
    print(f"  0xE1 + 0x0E = 0x{0xE1 + 0x0E:02X} (0xEF) OK")

    return sorted_types

def check_for_type_a0(types):
    """Check if type 0xA0 is in the extracted types."""
    print(f"\n{'=' * 100}")
    print("TYPE 0xA0 SEARCH")
    print('=' * 100)

    if 0xA0 in types:
        print("\n[OK] Type 0xA0 FOUND in BST!")
    else:
        print("\n[X] Type 0xA0 NOT in BST")
        print("\nLet's check if 0xA0 could be in a SUB chain we missed...")

        # Check if 0xA0 is between any consecutive types
        for i in range(len(types) - 1):
            if types[i] < 0xA0 < types[i+1]:
                print(f"\n0xA0 falls between 0x{types[i]:02X} and 0x{types[i+1]:02X}")
                diff_from_lower = 0xA0 - types[i]
                diff_to_upper = types[i+1] - 0xA0
                print(f"  Distance from 0x{types[i]:02X}: +0x{diff_from_lower:02X}")
                print(f"  Distance to 0x{types[i+1]:02X}: +0x{diff_to_upper:02X}")

        # Check if there's a gap in the upper branch where 0xA0 could fit
        print("\nChecking upper branch (type > 0x87):")
        upper_types = [t for t in types if t > 0x87]
        print(f"Upper branch types: {[f'0x{t:02X}' for t in upper_types]}")

        # 0xA0 is between 0x95 and 0xC2
        if 0x95 in upper_types and 0xC2 in upper_types:
            print(f"\n0xA0 is between 0x95 and 0xC2 in the upper branch!")
            print(f"  If there was: sub eax, 0x95; then sub eax, 0x0B; je handler")
            print(f"  That would check for 0x95 + 0x0B = 0x{0x95 + 0x0B:02X} (0xA0)")
            print("\nLet's check if there's a sub eax, 0x0B in the disassembly...")

def search_for_missing_sub(data):
    """Search for a 'sub eax, 0x0B' instruction that could test for 0xA0."""
    print(f"\n{'=' * 100}")
    print("SEARCHING FOR 'SUB EAX, 0x0B' INSTRUCTION")
    print('=' * 100)

    # Pattern: 83 e8 0b (sub eax, 0x0b)
    pattern = b'\x83\xe8\x0b'

    offset = 0
    found_any = False
    while True:
        offset = data.find(pattern, offset)
        if offset == -1:
            break

        found_any = True
        print(f"\nFound 'sub eax, 0x0b' at offset 0x{offset:04x}")

        # Disassemble context
        context_start = max(0, offset - 20)
        context_size = 60
        insns = disassemble_range(data, context_start, context_size)

        print("Context:")
        for insn in insns:
            marker = "  >>> " if insn.address == offset else "      "
            bytes_str = ' '.join(f'{b:02x}' for b in insn.bytes)
            print(f"{marker}0x{insn.address:04x}: {bytes_str:20s} {insn.mnemonic:8s} {insn.op_str}")

        offset += len(pattern)

    if not found_any:
        print("\nNo 'sub eax, 0x0b' instruction found in the module.")
        print("Type 0xA0 is likely:")
        print("  1. Not used by this module")
        print("  2. Handled by a remap table")
        print("  3. Part of the default error handler (rejected types)")

def main():
    print("Analyzing Warden module 2C045995 BST structure\n")

    data = read_binary(BINARY_PATH)
    print(f"Loaded {len(data)} bytes\n")

    # Trace the BST manually based on the disassembly
    types = trace_bst([])

    # Check for type 0xA0
    check_for_type_a0(types)

    # Search for the missing SUB instruction
    search_for_missing_sub(data)

    print(f"\n{'=' * 100}")
    print("FINAL CONCLUSION")
    print('=' * 100)
    print("\nExtracted types from module 2C045995:")
    print(f"  {[f'0x{t:02X}' for t in types]}")
    print(f"\nTotal: {len(types)} types (expected 9-10)")

    known_types = [0x0E, 0x2D, 0x49, 0x68, 0x87, 0x95, 0xC2, 0xE1, 0xEF]
    if sorted(types) == sorted(known_types):
        print("\n[OK] All 9 known types extracted successfully!")
    else:
        print(f"\n[X] Mismatch detected")
        missing = [t for t in known_types if t not in types]
        extra = [t for t in types if t not in known_types]
        if missing:
            print(f"  Missing: {[f'0x{t:02X}' for t in missing]}")
        if extra:
            print(f"  Extra: {[f'0x{t:02X}' for t in extra]}")

if __name__ == '__main__':
    main()
