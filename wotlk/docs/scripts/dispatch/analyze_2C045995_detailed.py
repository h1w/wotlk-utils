#!/usr/bin/env python3
"""
Detailed analysis of Warden module 2C045995 dispatch chain at 0x4AC2.
Extract all type comparisons and find the default case handler.
"""

from capstone import *

BINARY_PATH = r"C:\Users\bpqvg\source\repos\wotlk-utils\wotlk\docs\warden_dumps\warden_2C0459958D7BF292DD317F66F8611446_decompressed.bin"

KNOWN_TYPES = [0x0E, 0x2D, 0x49, 0x68, 0x87, 0x95, 0xC2, 0xE1, 0xEF]

def read_binary(path):
    with open(path, 'rb') as f:
        return f.read()

def disassemble_range(data, start_offset, size, base_addr=0):
    """Disassemble and return instructions."""
    md = Cs(CS_ARCH_X86, CS_MODE_32)
    md.detail = True
    instructions = []
    for insn in md.disasm(data[start_offset:start_offset+size], base_addr + start_offset):
        instructions.append(insn)
    return instructions

def print_insns(insns, title):
    """Print instructions with formatting."""
    print(f"\n{'=' * 100}")
    print(title)
    print('=' * 100)
    for insn in insns:
        bytes_str = ' '.join(f'{b:02x}' for b in insn.bytes)
        print(f"0x{insn.address:04x}:  {bytes_str:30s}  {insn.mnemonic:10s} {insn.op_str}")

def extract_dispatch_chain(instructions):
    """Extract all type comparisons from dispatch chain."""
    print(f"\n{'=' * 100}")
    print("DISPATCH CHAIN EXTRACTION")
    print('=' * 100)

    cmp_values = []
    jumps = []
    default_case = None

    for i, insn in enumerate(instructions):
        # Extract comparison values
        if insn.mnemonic == 'cmp' and len(insn.operands) == 2:
            op2 = insn.operands[1]
            if op2.type == 2:  # Immediate
                value = op2.imm
                if value < 0x100:  # Single byte
                    cmp_values.append(value)
                    print(f"[CMP] 0x{insn.address:04x}: cmp eax, 0x{value:02x}")

        # Track conditional jumps
        if insn.mnemonic.startswith('j') and insn.mnemonic != 'jmp':
            if len(insn.operands) > 0 and insn.operands[0].type == 2:
                target = insn.operands[0].imm
                jumps.append((insn.address, insn.mnemonic, target))
                print(f"      {insn.mnemonic:10s} -> 0x{target:04x}")

        # Look for unconditional jump (likely default case)
        # It should be a jmp that's NOT a handler branch (those are after je/jne)
        if insn.mnemonic == 'jmp':
            if len(insn.operands) > 0 and insn.operands[0].type == 2:
                target = insn.operands[0].imm
                # Check if previous instruction was a comparison failure path
                # (i.e., this jmp is reached when no cmp matched)
                if i > 0:
                    prev = instructions[i-1]
                    # If previous was NOT a conditional jump, this might be default
                    if not prev.mnemonic.startswith('j'):
                        print(f"\n[DEFAULT CASE?] 0x{insn.address:04x}: jmp -> 0x{target:04x}")
                        if default_case is None:
                            default_case = target
                    else:
                        print(f"[HANDLER] 0x{insn.address:04x}: jmp -> 0x{target:04x}")

    return cmp_values, default_case

def find_default_handler(data, instructions, xor_offset):
    """
    Find the default handler by tracing the fall-through path.
    In a BST dispatch, after all comparisons fail, there's usually a jne/jmp to default.
    """
    print(f"\n{'=' * 100}")
    print("TRACING DEFAULT HANDLER PATH")
    print('=' * 100)

    # Look for the last conditional jump in the chain
    # After all type comparisons, there should be a final jne/jmp to default handler
    last_jne = None
    last_jmp_unconditional = None

    for i, insn in enumerate(instructions):
        if insn.mnemonic == 'jne' or insn.mnemonic == 'jnz':
            if len(insn.operands) > 0 and insn.operands[0].type == 2:
                last_jne = (insn.address, insn.operands[0].imm)

        if insn.mnemonic == 'jmp':
            if len(insn.operands) > 0 and insn.operands[0].type == 2:
                # Only track jmps that are likely default (not handler exits)
                # Heuristic: jmp targets far ahead are likely error/default handlers
                target = insn.operands[0].imm
                if target > insn.address + 0x100:  # Far jump
                    last_jmp_unconditional = (insn.address, target)

    if last_jne:
        print(f"Last JNE: 0x{last_jne[0]:04x} -> 0x{last_jne[1]:04x} (likely default handler)")
        return last_jne[1]

    if last_jmp_unconditional:
        print(f"Last unconditional JMP: 0x{last_jmp_unconditional[0]:04x} -> 0x{last_jmp_unconditional[1]:04x}")
        return last_jmp_unconditional[1]

    return None

def disassemble_default_handler(data, offset, base_addr=0):
    """Disassemble the default handler to see if it has a sub-dispatcher or remap."""
    print(f"\n{'=' * 100}")
    print(f"DEFAULT HANDLER DISASSEMBLY (starting at 0x{offset:04x})")
    print('=' * 100)

    instructions = disassemble_range(data, offset, 200, base_addr)

    for insn in instructions:
        bytes_str = ' '.join(f'{b:02x}' for b in insn.bytes)
        print(f"0x{insn.address:04x}:  {bytes_str:30s}  {insn.mnemonic:10s} {insn.op_str}")

        # Look for remap table pattern: movzx reg, byte [table + reg]
        if insn.mnemonic == 'movzx' and len(insn.operands) == 2:
            op2 = insn.operands[1]
            if op2.size == 1:  # Loading a byte
                print(f"\n      ^^^ REMAP TABLE LOAD? Check next instructions for jmp [table]")

    return instructions

def main():
    print("Analyzing Warden module 2C045995 dispatch chain at 0x4AC2\n")

    data = read_binary(BINARY_PATH)
    print(f"Loaded {len(data)} bytes\n")

    # XOR site at 0x4AC2 is the dispatch chain
    xor_offset = 0x4AC2
    base_addr = 0x0

    # Disassemble from 50 bytes before to 500 bytes after to capture full BST
    disasm_start = max(0, xor_offset - 50)
    disasm_size = 550

    print(f"Disassembling 0x{disasm_start:04x} to 0x{disasm_start + disasm_size:04x}")

    instructions = disassemble_range(data, disasm_start, disasm_size, base_addr)
    print_insns(instructions, f"FULL DISPATCH CHAIN (XOR at 0x{xor_offset:04x})")

    # Extract dispatch chain
    cmp_values, default_from_jmp = extract_dispatch_chain(instructions)

    print(f"\n{'=' * 100}")
    print("SUMMARY")
    print('=' * 100)

    print(f"\nComparison values in dispatch chain: {[f'0x{v:02X}' for v in sorted(cmp_values)]}")
    print(f"Total types found: {len(cmp_values)}")

    # Check for missing types
    missing = [t for t in KNOWN_TYPES if t not in cmp_values]
    found_extra = [v for v in cmp_values if v not in KNOWN_TYPES]

    if found_extra:
        print(f"\nEXTRA types found (not in known list): {[f'0x{v:02X}' for v in found_extra]}")

    if missing:
        print(f"\nMISSING types (in known list but not in chain): {[f'0x{v:02X}' for v in missing]}")
        print("\nThese missing types must be handled by the default case.")

    # Find default handler
    default_offset = find_default_handler(data, instructions, xor_offset)

    if default_offset:
        print(f"\nDefault handler offset: 0x{default_offset:04x}")
        # Disassemble it
        default_insns = disassemble_default_handler(data, default_offset, base_addr)

        # Check if default handler has further comparisons
        print(f"\n{'=' * 100}")
        print("CHECKING DEFAULT HANDLER FOR SUB-DISPATCHER")
        print('=' * 100)

        sub_cmps = []
        for insn in default_insns:
            if insn.mnemonic == 'cmp' and len(insn.operands) == 2:
                op2 = insn.operands[1]
                if op2.type == 2 and op2.imm < 0x100:
                    sub_cmps.append(op2.imm)
                    print(f"[SUB-CMP] 0x{insn.address:04x}: cmp with 0x{op2.imm:02x}")

        if sub_cmps:
            print(f"\nFound sub-comparisons: {[f'0x{v:02X}' for v in sub_cmps]}")
            print("\n*** The default handler has a sub-dispatcher! ***")
            print(f"Missing types {[f'0x{v:02X}' for v in missing]} are likely in this sub-dispatcher.")
        else:
            print("\nNo sub-comparisons found in default handler.")
            print("It may use a remap table or be an error handler.")

    # Final check for 0xA0
    print(f"\n{'=' * 100}")
    print("TYPE 0xA0 ANALYSIS")
    print('=' * 100)

    if 0xA0 in cmp_values:
        print("Type 0xA0 FOUND in main dispatch chain!")
    elif default_offset and 0xA0 in sub_cmps:
        print("Type 0xA0 FOUND in default handler sub-dispatcher!")
    else:
        print("Type 0xA0 NOT FOUND in either main chain or default sub-dispatcher.")
        print("\nPossible explanations:")
        print("1. Type 0xA0 is remapped via a table in the default handler")
        print("2. Type 0xA0 is handled by a completely separate function")
        print("3. Type 0xA0 is not actually used by this module")

if __name__ == '__main__':
    main()
