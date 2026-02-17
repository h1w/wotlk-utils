#!/usr/bin/env python3
"""Check the binary file and both XOR sites."""

from capstone import *

BINARY_PATH = r"C:\Users\bpqvg\source\repos\wotlk-utils\wotlk\docs\warden_dumps\warden_2C0459958D7BF292DD317F66F8611446_decompressed.bin"

def hexdump(data, offset, size=64):
    """Print hexdump."""
    for i in range(0, size, 16):
        addr = offset + i
        hex_part = ' '.join(f'{data[addr + j]:02x}' if addr + j < len(data) else '  '
                           for j in range(16))
        ascii_part = ''.join(chr(data[addr + j]) if 32 <= data[addr + j] < 127 else '.'
                            for j in range(16) if addr + j < len(data))
        print(f"0x{addr:04x}:  {hex_part:48s}  {ascii_part}")

def try_disasm(data, offset, size=50):
    """Try to disassemble."""
    md = Cs(CS_ARCH_X86, CS_MODE_32)
    md.detail = True

    print(f"\nAttempting disassembly at 0x{offset:04x}:")
    count = 0
    for insn in md.disasm(data[offset:offset+size], offset):
        bytes_str = ' '.join(f'{b:02x}' for b in insn.bytes)
        print(f"0x{insn.address:04x}: {bytes_str:30s} {insn.mnemonic:10s} {insn.op_str}")
        count += 1
        if count > 20:
            break

    if count == 0:
        print("  [ERROR] No instructions could be disassembled!")
        print("  This may be data, not code.")

def main():
    with open(BINARY_PATH, 'rb') as f:
        data = f.read()

    print(f"Binary size: {len(data)} bytes (0x{len(data):x})")

    # Check XOR site 1 at 0x10D6
    print(f"\n{'=' * 80}")
    print("XOR SITE #1: 0x10D6")
    print('=' * 80)

    print("\nHexdump around 0x10D6:")
    hexdump(data, max(0, 0x10D6 - 16), 64)

    # Check if the pattern matches XOR instruction
    if data[0x10D6:0x10D6+3] == b'\x32\x46\x04':
        print("\n[OK] Pattern matches: 32 46 04 = xor al, byte ptr [esi+4]")
    else:
        print("\n[ERROR] Pattern does NOT match expected XOR instruction!")

    try_disasm(data, 0x10D6, 100)

    # Check XOR site 2 at 0x4AC2
    print(f"\n{'=' * 80}")
    print("XOR SITE #2: 0x4AC2")
    print('=' * 80)

    print("\nHexdump around 0x4AC2:")
    hexdump(data, max(0, 0x4AC2 - 16), 64)

    if data[0x4AC2:0x4AC2+3] == b'\x32\x5e\x04':
        print("\n[OK] Pattern matches: 32 5e 04 = xor bl, byte ptr [esi+4]")
    else:
        print("\n[ERROR] Pattern does NOT match expected XOR instruction!")

    try_disasm(data, 0x4AC2, 100)

if __name__ == '__main__':
    main()
