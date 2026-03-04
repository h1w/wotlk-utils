"""
Disassemble questgiver (QUEST_GREETING) Lua C handlers from Wow.exe (build 12340)
to reverse-engineer the internal questgiver quest data structure.

These functions are used when an NPC shows QUEST_GREETING instead of GOSSIP_SHOW.
The gossip array at 0x00BFC968 is NOT populated in this case — a separate
questgiver array is used, populated by SMSG_QUESTGIVER_QUEST_LIST (opcode 0x18D).

Strategy:
  1. Scan .rdata for known Lua registration strings (GetAvailableTitle, etc.)
  2. Find their addresses as pointers inside a luaL_Reg table
  3. Collect the adjacent function pointers
  4. Disassemble each function to find the questgiver array base + struct layout
"""

import re
import struct
import pefile
import capstone

WOW_EXE = r"Z:\Games\wow 3.3.5a client\Wow.exe"

# Lua function names we expect to find for QUEST_GREETING
TARGET_NAMES = [
    "GetNumAvailableQuests",
    "GetAvailableTitle",
    "GetNumActiveQuests",
    "GetActiveTitle",
    "SelectAvailableQuest",
    "SelectActiveQuest",
    "CloseQuest",
    "AcceptQuest",
    "GetTitleText",
    "GetQuestID",
    "GetGreetingText",
]

# Extra known VA addresses to always disassemble (from initial guess / cross-refs)
# Leave empty; we fill dynamically from the string scan.
EXTRA_TARGETS: dict[str, tuple[int, int]] = {}


# ---------------------------------------------------------------------------
# PE helpers
# ---------------------------------------------------------------------------

def load_pe(path):
    pe = pefile.PE(path, fast_load=True)
    with open(path, "rb") as f:
        data = f.read()
    return pe, data


def section_for_rva(pe, rva):
    for s in pe.sections:
        start = s.VirtualAddress
        end = start + s.Misc_VirtualSize
        if start <= rva < end:
            return s
    return None


def va_to_offset(pe, va):
    rva = va - pe.OPTIONAL_HEADER.ImageBase
    s = section_for_rva(pe, rva)
    if s is None:
        return None
    return s.PointerToRawData + (rva - s.VirtualAddress)


def read_at_va(data, pe, va, size):
    off = va_to_offset(pe, va)
    if off is None:
        return None
    return data[off:off + size]


def read_u32_at(data, offset):
    return struct.unpack_from("<I", data, offset)[0]


# ---------------------------------------------------------------------------
# Find Lua registration table entries
# ---------------------------------------------------------------------------

def find_string_va(data, pe, needle: bytes):
    """Return list of VAs where `needle` appears in any section."""
    result = []
    start = 0
    while True:
        pos = data.find(needle, start)
        if pos < 0:
            break
        # Convert file offset → VA
        for s in pe.sections:
            raw_start = s.PointerToRawData
            raw_end = raw_start + s.SizeOfRawData
            if raw_start <= pos < raw_end:
                rva = s.VirtualAddress + (pos - raw_start)
                result.append(pe.OPTIONAL_HEADER.ImageBase + rva)
                break
        start = pos + 1
    return result


def search_lua_registrations(data, pe, names: list[str]):
    """
    For each name string, find its VA(s) in the binary, then search
    all .rdata / .data sections for a 4-byte LE value equal to that VA.
    The 4 bytes immediately before (name) or after (func) give us the
    function pointer, depending on luaL_Reg layout {name*, func*}.

    Returns: dict  name → list of candidate func VAs
    """
    image_base = pe.OPTIONAL_HEADER.ImageBase
    # Collect sections that may hold pointer tables (.rdata, .data)
    ptr_sections = []
    for s in pe.sections:
        sec_name = s.Name.rstrip(b"\x00").decode("ascii", errors="replace")
        if sec_name in (".rdata", ".data"):
            ptr_sections.append(s)

    results = {}

    for name in names:
        needle = name.encode("ascii") + b"\x00"
        str_vas = find_string_va(data, pe, needle)
        if not str_vas:
            print(f"  [!] String not found: {name!r}")
            continue

        func_vas = []
        for str_va in str_vas:
            # Search for ptr to this string in pointer-holding sections
            target = struct.pack("<I", str_va)
            for s in ptr_sections:
                raw_s = s.PointerToRawData
                raw_e = raw_s + s.SizeOfRawData
                sec_data = data[raw_s:raw_e]
                off = 0
                while True:
                    pos = sec_data.find(target, off)
                    if pos < 0:
                        break
                    # Align to 4 bytes (Lua tables are 4-byte aligned)
                    if pos % 4 == 0:
                        # luaL_Reg: {const char* name, lua_CFunction func}
                        # name ptr is at pos → func ptr at pos+4
                        if pos + 8 <= len(sec_data):
                            func_ptr = read_u32_at(sec_data, pos + 4)
                            if 0x00400000 <= func_ptr <= 0x00FFFFFF:
                                func_vas.append(func_ptr)
                        # Also check: func ptr at pos-4 → name ptr at pos
                        if pos >= 4:
                            func_ptr = read_u32_at(sec_data, pos - 4)
                            if 0x00400000 <= func_ptr <= 0x00FFFFFF:
                                func_vas.append(func_ptr)
                    off = pos + 1

        func_vas = list(dict.fromkeys(func_vas))  # deduplicate, preserve order
        results[name] = func_vas
        if func_vas:
            print(f"  [+] {name!r}: func VA candidates = {[hex(v) for v in func_vas]}")
        else:
            print(f"  [?] {name!r}: string found at {[hex(v) for v in str_vas]} but no func pointer found")

    return results


# ---------------------------------------------------------------------------
# Disassembly
# ---------------------------------------------------------------------------

def disasm_function(md, data, pe, va, name, max_size=0x200):
    code = read_at_va(data, pe, va, max_size)
    if code is None:
        print(f"  [!] Cannot read bytes at VA 0x{va:08X}")
        return []

    print(f"\n{'='*70}")
    print(f"  {name} @ 0x{va:08X}")
    print(f"{'='*70}")

    lines = []
    ret_count = 0
    for insn in md.disasm(code, va):
        line = f"  0x{insn.address:08X}:  {insn.mnemonic:<8s} {insn.op_str}"
        lines.append((insn, line))
        print(line)
        if insn.mnemonic in ("ret", "retn"):
            ret_count += 1
            if ret_count >= 1:
                break
        if insn.mnemonic == "int3":
            break

    return lines


def extract_global_refs(lines):
    """Pull out immediate memory addresses in .data/.bss range from disasm lines."""
    refs = []
    for insn, line in lines:
        matches = re.findall(r"\[0x([0-9a-fA-F]{6,8})\]", line)
        for m in matches:
            addr = int(m, 16)
            if 0x00800000 <= addr <= 0x01200000:
                refs.append((addr, line.strip()))
        matches2 = re.findall(r"(?:mov|lea|push|cmp|add|sub|imul).*?0x([0-9a-fA-F]{6,8})", line)
        for m in matches2:
            addr = int(m, 16)
            if 0x00800000 <= addr <= 0x01200000:
                refs.append((addr, line.strip()))
    return refs


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main():
    print(f"Loading {WOW_EXE}...")
    pe, data = load_pe(WOW_EXE)
    image_base = pe.OPTIONAL_HEADER.ImageBase
    print(f"Image base: 0x{image_base:08X}")

    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    md.detail = False

    # Step 1: Find Lua C handler addresses by scanning string tables
    print("\n--- Scanning for Lua registration strings ---")
    name_to_funcs = search_lua_registrations(data, pe, TARGET_NAMES)

    # Step 2: Disassemble every found handler
    all_globals: dict[int, list[tuple[str, str]]] = {}

    targets: dict[str, int] = {}  # name → best VA to disassemble

    for name, func_vas in name_to_funcs.items():
        for va in func_vas:
            targets[f"lua_{name}"] = va
            break  # use first candidate; others printed for reference

    for name, (va, max_sz) in EXTRA_TARGETS.items():
        targets[name] = va

    for fname, va in sorted(targets.items(), key=lambda x: x[1]):
        lines = disasm_function(md, data, pe, va, fname)
        for addr, ctx in extract_global_refs(lines):
            all_globals.setdefault(addr, []).append((fname, ctx))

    # Step 3: Summary
    print(f"\n{'='*70}")
    print("  GLOBAL/STATIC ADDRESS REFERENCES SUMMARY")
    print(f"{'='*70}")
    for addr in sorted(all_globals.keys()):
        refs = all_globals[addr]
        print(f"\n  0x{addr:08X}  (referenced by {len(refs)} function(s)):")
        for func_name, ctx in refs:
            print(f"    [{func_name}] {ctx}")

    print("\nDone.")


if __name__ == "__main__":
    main()
