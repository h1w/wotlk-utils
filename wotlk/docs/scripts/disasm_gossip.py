"""
Disassemble CGQuestInfo gossip-related functions from Wow.exe (build 12340)
to reverse-engineer the internal gossip quest data structure.

Target functions:
  0x0058A5D0  GetNumAvailGossipQuests
  0x0058A660  GetAvailableQuestInfoFromIndex
  0x0058A6C0  GetNumActiveGossipQuests
  0x0058A750  GetActiveQuestFromIndex
  0x0058B1B0  SMSG_GOSSIP_MESSAGE packet handler
  0x0058B3A0  lua_GetGossipAvailableQuests
  0x0058B490  lua_GetGossipActiveQuests
  0x0058A550  SetGossipObjectGUID
"""

import sys
import pefile
import capstone

WOW_EXE = r"Z:\Games\wow 3.3.5a client\Wow.exe"

# Functions to disassemble with expected max size
TARGETS = {
    "SetGossipObjectGUID":            (0x0058A550, 0x80),
    "GetNumAvailGossipQuests":        (0x0058A5D0, 0x90),
    "GetAvailableQuestInfoFromIndex": (0x0058A660, 0x60),
    "GetNumActiveGossipQuests":       (0x0058A6C0, 0x90),
    "GetActiveQuestFromIndex":        (0x0058A750, 0x60),
    "IsLowLevel":                     (0x0058ABA0, 0x60),
    "SMSG_GOSSIP_MESSAGE":            (0x0058B1B0, 0x200),
    "lua_GetGossipAvailableQuests":   (0x0058B3A0, 0x100),
    "lua_GetGossipActiveQuests":      (0x0058B490, 0x100),
    "lua_GetNumGossipAvailableQuests":(0x0058A960, 0x40),
    "lua_GetNumGossipActiveQuests":   (0x0058A9A0, 0x40),
    "SelectGossipAvailableQuest":     (0x0058B070, 0xE0),
    "SelectGossipActiveQuest":        (0x0058B670, 0xE0),  # approximate
}


def load_wow_exe(path):
    pe = pefile.PE(path, fast_load=True)
    image_base = pe.OPTIONAL_HEADER.ImageBase
    # Find .text section
    for section in pe.sections:
        name = section.Name.rstrip(b'\x00').decode('ascii', errors='replace')
        if name == '.text':
            return pe, image_base, section
    raise RuntimeError(".text section not found")


def va_to_file_offset(va, image_base, text_section):
    rva = va - image_base
    sec_rva = text_section.VirtualAddress
    sec_end = sec_rva + text_section.Misc_VirtualSize
    if rva < sec_rva or rva >= sec_end:
        # Try .rdata section
        return None
    return text_section.PointerToRawData + (rva - sec_rva)


def read_bytes_at_va(pe_data, va, size, image_base, text_section):
    offset = va_to_file_offset(va, image_base, text_section)
    if offset is None:
        return None
    return pe_data[offset:offset + size]


def disassemble_function(md, code, start_va, name, max_size):
    """Disassemble until RET or max_size, printing each instruction."""
    print(f"\n{'='*70}")
    print(f"  {name} @ 0x{start_va:08X}")
    print(f"{'='*70}")

    lines = []
    for insn in md.disasm(code[:max_size], start_va):
        line = f"  0x{insn.address:08X}:  {insn.mnemonic:<8s} {insn.op_str}"
        lines.append(line)
        print(line)
        # Stop at ret or int3 (function boundary)
        if insn.mnemonic == 'ret':
            break
        if insn.mnemonic == 'int3':
            break

    return lines


def find_global_refs(lines):
    """Extract references to global/static addresses (immediate memory operands)."""
    import re
    globals_found = []
    for line in lines:
        # Look for patterns like [0x00XXXXXX] — direct memory references
        matches = re.findall(r'\[0x([0-9a-f]{6,8})\]', line, re.IGNORECASE)
        for m in matches:
            addr = int(m, 16)
            if 0x00800000 <= addr <= 0x01000000:  # .data/.bss range
                globals_found.append((addr, line.strip()))
        # Also look for immediate large values (possible global pointers)
        matches = re.findall(r'(?:mov|lea|push|cmp).*?,?\s*0x([0-9a-f]{6,8})', line, re.IGNORECASE)
        for m in matches:
            addr = int(m, 16)
            if 0x00800000 <= addr <= 0x01000000:
                globals_found.append((addr, line.strip()))

    return globals_found


def main():
    print(f"Loading {WOW_EXE}...")

    pe, image_base, text_section = load_wow_exe(WOW_EXE)
    print(f"Image base: 0x{image_base:08X}")
    print(f".text: RVA=0x{text_section.VirtualAddress:08X}, "
          f"VSize=0x{text_section.Misc_VirtualSize:08X}, "
          f"RawOff=0x{text_section.PointerToRawData:08X}")

    # Read entire file into memory for fast access
    with open(WOW_EXE, 'rb') as f:
        pe_data = f.read()

    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    md.detail = False

    all_globals = {}

    for name, (va, max_size) in TARGETS.items():
        code = read_bytes_at_va(pe_data, va, max_size, image_base, text_section)
        if code is None:
            print(f"\n[WARN] {name} @ 0x{va:08X} is outside .text section")
            # Try reading from any section
            for section in pe.sections:
                sec_rva = section.VirtualAddress
                sec_end = sec_rva + section.Misc_VirtualSize
                rva = va - image_base
                if sec_rva <= rva < sec_end:
                    offset = section.PointerToRawData + (rva - sec_rva)
                    code = pe_data[offset:offset + max_size]
                    break
            if code is None:
                continue

        lines = disassemble_function(md, code, va, name, max_size)

        # Collect global references
        refs = find_global_refs(lines)
        for addr, context in refs:
            if addr not in all_globals:
                all_globals[addr] = []
            all_globals[addr].append((name, context))

    # Summary: all global addresses referenced
    print(f"\n{'='*70}")
    print("  GLOBAL/STATIC ADDRESS REFERENCES")
    print(f"{'='*70}")

    for addr in sorted(all_globals.keys()):
        refs = all_globals[addr]
        print(f"\n  0x{addr:08X}  (referenced by {len(refs)} function(s)):")
        for func_name, context in refs:
            print(f"    [{func_name}] {context}")


if __name__ == "__main__":
    main()
