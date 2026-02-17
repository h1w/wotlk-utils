#!/usr/bin/env python3
"""
Analyze XOR site at 0x10D6 in module 2C045995 to see if it's a response builder
with a remap table that contains type 0xA0.
"""

from capstone import *
import struct

BINARY_PATH = r"C:\Users\bpqvg\source\repos\wotlk-utils\wotlk\docs\warden_dumps\warden_2C0459958D7BF292DD317F66F8611446_decompressed.bin"

def read_binary(path):
    with open(path, 'rb') as f:
        return f.read()

def disassemble_range(data, start_offset, size, base_addr=0):
    md = Cs(CS_ARCH_X86, CS_MODE_32)
    md.detail = True
    return list(md.disasm(data[start_offset:start_offset+size], base_addr + start_offset))

def find_remap_table(data, instructions):
    """
    Look for remap table pattern:
      movzx reg, byte ptr [table_offset + index_reg]
      jmp dword ptr [jtable + reg*4]

    Returns (remap_table_offset, table_size) or None.
    """
    for i, insn in enumerate(instructions):
        if insn.mnemonic == 'movzx' and len(insn.operands) == 2:
            # Check if loading a byte from memory
            op1 = insn.operands[0]
            op2 = insn.operands[1]

            if op2.size == 1 and op2.type == 3:  # X86_OP_MEM, byte load
                # Check if memory operand has displacement (table offset)
                mem = op2.mem
                if mem.disp != 0:
                    table_offset = mem.disp
                    print(f"\n[REMAP?] 0x{insn.address:04x}: {insn.mnemonic} {insn.op_str}")
                    print(f"         Possible remap table at offset 0x{table_offset:04x}")

                    # Look ahead for indirect jmp
                    for j in range(i+1, min(i+10, len(instructions))):
                        next_insn = instructions[j]
                        if next_insn.mnemonic == 'jmp':
                            if len(next_insn.operands) > 0:
                                op = next_insn.operands[0]
                                if op.type == 3:  # X86_OP_MEM - indirect jump
                                    print(f"         Followed by indirect jmp at 0x{next_insn.address:04x}")
                                    print(f"\n         FOUND REMAP TABLE PATTERN!")
                                    return table_offset

    return None

def extract_remap_table(data, offset, max_size=256):
    """
    Extract the remap table starting at offset.
    The table maps input type IDs to output type IDs.

    Returns dict: {input_id: output_id}
    """
    print(f"\n{'=' * 100}")
    print(f"EXTRACTING REMAP TABLE FROM OFFSET 0x{offset:04x}")
    print('=' * 100)

    remap = {}

    # Read up to max_size bytes
    for i in range(max_size):
        if offset + i >= len(data):
            break

        input_id = i
        output_id = data[offset + i]

        # Stop at end marker (common patterns: 0xFF, 0x00 repeating)
        if output_id == 0xFF and i > 10:
            # Check if rest is all 0xFF
            if all(data[offset + j] == 0xFF for j in range(i, min(i+10, max_size))):
                print(f"\nEnd of table detected at entry {i} (0xFF padding)")
                break

        # Only record valid mappings (non-zero or explicitly mapped to zero)
        if output_id != 0 or i == 0:
            remap[input_id] = output_id

    print(f"\nExtracted {len(remap)} remap entries")

    return remap

def analyze_remap_table(remap, known_types):
    """Analyze the remap table to find which input IDs map to known output types."""
    print(f"\n{'=' * 100}")
    print("REMAP TABLE ANALYSIS")
    print('=' * 100)

    # Group by output type
    reverse_map = {}
    for input_id, output_id in remap.items():
        if output_id not in reverse_map:
            reverse_map[output_id] = []
        reverse_map[output_id].append(input_id)

    print("\nReverse mapping (output type -> list of input IDs):")
    for output_id in sorted(reverse_map.keys()):
        inputs = reverse_map[output_id]
        known_marker = " <-- KNOWN TYPE" if output_id in known_types else ""
        print(f"  0x{output_id:02X} <- {[f'0x{i:02X}' for i in inputs]}{known_marker}")

    # Check if any input maps to the known types
    print("\n\nChecking which input IDs produce known output types:")
    for known_type in sorted(known_types):
        if known_type in reverse_map:
            inputs = reverse_map[known_type]
            print(f"  Known type 0x{known_type:02X} can be produced by: {[f'0x{i:02X}' for i in inputs]}")

    # Check specifically for 0xA0
    print(f"\n{'=' * 100}")
    print("TYPE 0xA0 IN REMAP TABLE")
    print('=' * 100)

    if 0xA0 in remap:
        output = remap[0xA0]
        print(f"\n[FOUND] Input 0xA0 maps to output 0x{output:02X}")
        if output in known_types:
            print(f"        This is a KNOWN type!")
            print(f"        Type 0xA0 is a REMAPPED type, internally handled as 0x{output:02X}")
        else:
            print(f"        This is NOT in the known types list")
    else:
        print("\n[NOT FOUND] Input 0xA0 does not appear in the remap table")

    if 0xA0 in reverse_map:
        inputs = reverse_map[0xA0]
        print(f"\n[FOUND] Output 0xA0 is produced by inputs: {[f'0x{i:02X}' for i in inputs]}")
    else:
        print("\n[NOT FOUND] Output 0xA0 is not produced by any remap entry")

def main():
    print("Analyzing XOR site at 0x10D6 for remap table (module 2C045995)\n")

    data = read_binary(BINARY_PATH)
    print(f"Loaded {len(data)} bytes\n")

    xor_offset = 0x10D6
    known_types = [0x0E, 0x2D, 0x49, 0x68, 0x87, 0x95, 0xC2, 0xE1, 0xEF]

    # Disassemble around the XOR site
    disasm_start = max(0, xor_offset - 30)
    disasm_size = 300

    print(f"Disassembling 0x{disasm_start:04x} to 0x{disasm_start + disasm_size:04x}")
    instructions = disassemble_range(data, disasm_start, disasm_size)

    print(f"\n{'=' * 100}")
    print("DISASSEMBLY AROUND XOR SITE 0x10D6")
    print('=' * 100)

    for insn in instructions[:50]:  # Print first 50 instructions
        marker = "  >>> " if insn.address == xor_offset else "      "
        bytes_str = ' '.join(f'{b:02x}' for b in insn.bytes)
        print(f"{marker}0x{insn.address:04x}: {bytes_str:30s} {insn.mnemonic:10s} {insn.op_str}")

    # Look for remap table
    table_offset = find_remap_table(data, instructions)

    if table_offset:
        # Extract the remap table
        remap = extract_remap_table(data, table_offset)

        # Analyze it
        analyze_remap_table(remap, known_types)

        # Print first 32 entries for inspection
        print(f"\n{'=' * 100}")
        print("FIRST 32 REMAP TABLE ENTRIES")
        print('=' * 100)
        for i in range(min(32, len(remap))):
            if i in remap:
                print(f"  0x{i:02X} -> 0x{remap[i]:02X}")
    else:
        print("\n[NOT FOUND] No remap table pattern detected at XOR site 0x10D6")
        print("\nThis site may be:")
        print("  1. A different type of XOR operation (not type ID remapping)")
        print("  2. Part of encryption/decryption logic")
        print("  3. A request parser without remap (using dispatch chain only)")

if __name__ == '__main__':
    main()
