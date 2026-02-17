#!/usr/bin/env python3
"""Quick dump of XOR site at 0x10D6."""

from capstone import *

BINARY_PATH = r"C:\Users\bpqvg\source\repos\wotlk-utils\wotlk\docs\warden_dumps\warden_2C0459958D7BF292DD317F66F8611446_decompressed.bin"

def main():
    with open(BINARY_PATH, 'rb') as f:
        data = f.read()

    md = Cs(CS_ARCH_X86, CS_MODE_32)
    md.detail = True

    # Check XOR site 0x10D6
    print("\nXOR site at 0x10D6:")
    print("Bytes:", ' '.join(f'{data[0x10D6 + i]:02x}' for i in range(10)))

    # Disassemble from 0x10C0 to 0x1150
    start = 0x10C0
    size = 0x90

    print(f"\nDisassembly 0x{start:04x} to 0x{start+size:04x}:\n")

    for insn in md.disasm(data[start:start+size], start):
        marker = ">>> " if insn.address == 0x10D6 else "    "
        bytes_str = ' '.join(f'{b:02x}' for b in insn.bytes)
        print(f"{marker}0x{insn.address:04x}: {bytes_str:30s} {insn.mnemonic:10s} {insn.op_str}")

if __name__ == '__main__':
    main()
