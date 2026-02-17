#!/usr/bin/env python3
"""
Disassemble Warden module 2C045995 dispatch chain to find missing type 0xA0.
"""

from capstone import *
import struct

BINARY_PATH = r"C:\Users\bpqvg\source\repos\wotlk-utils\wotlk\docs\warden_dumps\warden_2C0459958D7BF292DD317F66F8611446_decompressed.bin"

# Known types from the module
KNOWN_TYPES = [0x0E, 0x2D, 0x49, 0x68, 0x87, 0x95, 0xC2, 0xE1, 0xEF]

def read_binary(path):
    """Read the decompressed Warden module binary."""
    with open(path, 'rb') as f:
        return f.read()

def disassemble_region(data, offset, base_addr, count=100):
    """Disassemble a region of code."""
    md = Cs(CS_ARCH_X86, CS_MODE_32)
    md.detail = True

    instructions = []
    for insn in md.disasm(data[offset:offset+count], base_addr + offset):
        instructions.append(insn)

    return instructions

def print_instructions(instructions, title):
    """Pretty print instructions."""
    print(f"\n{'=' * 80}")
    print(f"{title}")
    print('=' * 80)

    for insn in instructions:
        bytes_str = ' '.join(f'{b:02x}' for b in insn.bytes)
        print(f"0x{insn.address:04x}:  {bytes_str:24s}  {insn.mnemonic:8s} {insn.op_str}")

def analyze_dispatch_chain(instructions, start_offset):
    """Analyze the dispatch chain to find type comparisons and default case."""
    print(f"\n{'=' * 80}")
    print("DISPATCH CHAIN ANALYSIS")
    print('=' * 80)

    comparisons = []
    jumps = []
    default_target = None

    for i, insn in enumerate(instructions):
        # Look for cmp/sub/dec instructions
        if insn.mnemonic in ['cmp', 'sub', 'dec']:
            # Try to extract immediate value
            if insn.mnemonic == 'cmp' and len(insn.operands) == 2:
                op2 = insn.operands[1]
                if op2.type == 2:  # X86_OP_IMM
                    comparisons.append((insn.address, op2.imm, insn))
                    print(f"\n[COMPARE] 0x{insn.address:04x}: cmp with 0x{op2.imm:02x}")
            elif insn.mnemonic == 'sub' and len(insn.operands) == 2:
                op2 = insn.operands[1]
                if op2.type == 2:  # X86_OP_IMM
                    comparisons.append((insn.address, op2.imm, insn))
                    print(f"[COMPARE] 0x{insn.address:04x}: sub with 0x{op2.imm:02x}")
            elif insn.mnemonic == 'dec':
                print(f"[COMPARE] 0x{insn.address:04x}: dec (comparing with 1)")

        # Look for conditional jumps
        if insn.mnemonic.startswith('j') and insn.mnemonic not in ['jmp']:
            if len(insn.operands) > 0 and insn.operands[0].type == 2:
                target = insn.operands[0].imm
                jumps.append((insn.address, insn.mnemonic, target))
                print(f"[COND_JUMP] 0x{insn.address:04x}: {insn.mnemonic:8s} -> 0x{target:04x}")

        # Look for unconditional jumps (potential default case)
        if insn.mnemonic == 'jmp':
            if len(insn.operands) > 0 and insn.operands[0].type == 2:
                target = insn.operands[0].imm
                # If this is after comparisons, it might be the default case
                if comparisons:
                    print(f"\n[DEFAULT?] 0x{insn.address:04x}: jmp -> 0x{target:04x} (possible default handler)")
                    if default_target is None:
                        default_target = target

    print(f"\n\nSummary:")
    print(f"  Found {len(comparisons)} comparisons")
    print(f"  Found {len(jumps)} conditional jumps")
    print(f"  Possible default target: 0x{default_target:04x}" if default_target else "  No default target found")

    # Extract comparison values
    cmp_values = [cmp[1] for cmp in comparisons if cmp[1] < 0x100]
    print(f"\n  Comparison values: {[f'0x{v:02x}' for v in cmp_values]}")

    # Check for missing types
    missing = [t for t in KNOWN_TYPES if t not in cmp_values]
    if missing:
        print(f"  MISSING from chain: {[f'0x{v:02x}' for v in missing]}")

    return default_target, cmp_values

def find_remap_table(data, instructions, start_offset):
    """Look for remap table patterns (movzx byte from table, jmp indirect)."""
    print(f"\n{'=' * 80}")
    print("REMAP TABLE SEARCH")
    print('=' * 80)

    for i, insn in enumerate(instructions):
        # Look for: movzx reg, byte ptr [offset+reg]
        if insn.mnemonic == 'movzx' and len(insn.operands) == 2:
            op1 = insn.operands[0]
            op2 = insn.operands[1]

            # Check if loading a byte
            if op2.size == 1:
                print(f"\n[REMAP?] 0x{insn.address:04x}: {insn.mnemonic} {insn.op_str}")

                # Check next few instructions for indirect jump
                for j in range(i+1, min(i+5, len(instructions))):
                    next_insn = instructions[j]
                    if next_insn.mnemonic == 'jmp':
                        # Check if indirect jump
                        if len(next_insn.operands) > 0:
                            op = next_insn.operands[0]
                            if op.type == 3:  # X86_OP_MEM (memory operand)
                                print(f"  [MATCH] Followed by indirect jump at 0x{next_insn.address:04x}")
                                print(f"         Full sequence:")
                                for k in range(i, j+1):
                                    seq_insn = instructions[k]
                                    print(f"         0x{seq_insn.address:04x}: {seq_insn.mnemonic:8s} {seq_insn.op_str}")
                                return True

    print("\nNo remap table pattern found in this region.")
    return False

def analyze_function_entry(data, xor_offset, base_addr=0):
    """Try to find function entry by looking backward for common prologues."""
    print(f"\n{'=' * 80}")
    print(f"SEARCHING FOR FUNCTION ENTRY (looking back from 0x{xor_offset:04x})")
    print('=' * 80)

    # Common function prologues
    prologues = [
        b'\x55\x8b\xec',           # push ebp; mov ebp, esp
        b'\x55\x89\xe5',           # push ebp; mov ebp, esp (AT&T style encoding)
        b'\x83\xec',               # sub esp, imm8
        b'\x81\xec',               # sub esp, imm32
        b'\x53',                   # push ebx
        b'\x56',                   # push esi
        b'\x57',                   # push edi
    ]

    # Look back up to 50 bytes
    search_start = max(0, xor_offset - 50)

    for offset in range(xor_offset - 1, search_start, -1):
        # Check for prologue patterns
        if data[offset:offset+3] == b'\x55\x8b\xec':
            print(f"Found 'push ebp; mov ebp, esp' at 0x{offset:04x} ({xor_offset - offset} bytes before XOR)")
            return offset
        elif data[offset:offset+2] == b'\x55\x89':
            print(f"Found possible prologue at 0x{offset:04x} ({xor_offset - offset} bytes before XOR)")
            return offset

    # If no clear prologue, start 20 bytes before XOR
    fallback = max(0, xor_offset - 20)
    print(f"No clear prologue found, starting at 0x{fallback:04x} (20 bytes before XOR)")
    return fallback

def main():
    print("Loading Warden module 2C045995...")
    data = read_binary(BINARY_PATH)
    print(f"Loaded {len(data)} bytes")

    # XOR sites
    xor_site_1 = 0x10D6
    xor_site_2 = 0x4AC2

    base_addr = 0x0  # Use 0 as base, offsets will match file offsets

    # ========== ANALYZE FIRST XOR SITE (0x10D6) ==========
    print(f"\n\n{'#' * 80}")
    print(f"# ANALYZING XOR SITE #1 at offset 0x{xor_site_1:04x}")
    print('#' * 80)

    # Find function entry
    entry_offset = analyze_function_entry(data, xor_site_1, base_addr)

    # Disassemble from entry to +300 bytes after XOR site
    disasm_start = entry_offset
    disasm_size = (xor_site_1 - entry_offset) + 300

    instructions_1 = disassemble_region(data, disasm_start, base_addr, disasm_size)
    print_instructions(instructions_1, f"DISASSEMBLY: 0x{disasm_start:04x} to 0x{disasm_start + disasm_size:04x}")

    # Analyze dispatch chain
    default_target_1, cmp_values_1 = analyze_dispatch_chain(instructions_1, disasm_start)

    # Look for remap table
    has_remap_1 = find_remap_table(data, instructions_1, disasm_start)

    # ========== ANALYZE SECOND XOR SITE (0x4AC2) ==========
    print(f"\n\n{'#' * 80}")
    print(f"# ANALYZING XOR SITE #2 at offset 0x{xor_site_2:04x}")
    print('#' * 80)

    # Disassemble 50 bytes before and after
    disasm_start_2 = max(0, xor_site_2 - 50)
    disasm_size_2 = 150

    instructions_2 = disassemble_region(data, disasm_start_2, base_addr, disasm_size_2)
    print_instructions(instructions_2, f"DISASSEMBLY: 0x{disasm_start_2:04x} to 0x{disasm_start_2 + disasm_size_2:04x}")

    # Check if this is a response builder (remap table)
    has_remap_2 = find_remap_table(data, instructions_2, disasm_start_2)

    # ========== FINAL SUMMARY ==========
    print(f"\n\n{'#' * 80}")
    print("# FINAL SUMMARY")
    print('#' * 80)

    print(f"\nXOR Site #1 (0x{xor_site_1:04x}):")
    print(f"  Comparison values found: {[f'0x{v:02x}' for v in cmp_values_1]}")
    print(f"  Has remap table: {has_remap_1}")
    print(f"  Default target: 0x{default_target_1:04x}" if default_target_1 else "  No default target")

    print(f"\nXOR Site #2 (0x{xor_site_2:04x}):")
    print(f"  Has remap table: {has_remap_2}")
    print(f"  Likely function: {'Response builder (remap)' if has_remap_2 else 'Request parser (dispatch chain)'}")

    # Check for type 0xA0
    if 0xA0 not in cmp_values_1:
        print(f"\n[CRITICAL] Type 0xA0 NOT found in dispatch chain at site #1")
        print(f"           This type is likely handled by the default case")
        if default_target_1:
            print(f"           Default handler is at 0x{default_target_1:04x}")
            print(f"           Recommendation: Disassemble the default handler to see if it")
            print(f"                          uses a remap table or sub-dispatcher")

if __name__ == '__main__':
    main()
