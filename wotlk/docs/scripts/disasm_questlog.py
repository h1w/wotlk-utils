"""
Scan Wow.exe (build 12340) for quest log Lua function registrations and
disassemble them to find the quest log memory layout:
  - Quest log array base address
  - Stride per entry
  - Offset of questId within each entry
  - Count global address

Strategy (same as disasm_questgiver.py):
  1. Scan .rdata for null-terminated Lua function name strings
  2. For each found string, look at [pos+4] as a possible function VA
  3. Disassemble the function to extract memory references
"""

import struct
import re
import pefile
import capstone

WOW_EXE = r"Z:\Games\wow 3.3.5a client\Wow.exe"

# Lua function names registered by the quest log system in build 12340
TARGET_STRINGS = [
    b"GetNumQuestLogEntries",
    b"GetQuestLogTitle",
    b"GetQuestLogQuestText",
    b"GetQuestLogRequiredMoney",
    b"GetQuestLogRewardMoney",
    b"GetQuestLogGroupNum",
    b"GetQuestLogLeadIn",
    b"SelectQuestLogEntry",
    b"AbandonQuest",
    b"CollapseQuestHeader",
    b"ExpandQuestHeader",
    b"GetQuestLogRewardInfo",
    b"GetQuestLogNumObjectives",
    b"GetQuestLogObjectiveText",
    b"IsCurrentQuestFailed",
    b"IsQuestWatched",
]

IMAGE_BASE = 0x00400000


def load_sections(path):
    pe = pefile.PE(path, fast_load=True)
    with open(path, "rb") as f:
        raw = f.read()
    sections = {}
    for s in pe.sections:
        name = s.Name.rstrip(b"\x00").decode("ascii", errors="replace")
        sections[name] = {
            "rva":    s.VirtualAddress,
            "vsize":  s.Misc_VirtualSize,
            "offset": s.PointerToRawData,
            "size":   s.SizeOfRawData,
        }
    return raw, pe.OPTIONAL_HEADER.ImageBase, sections


def va_to_raw(va, image_base, sections):
    rva = va - image_base
    for sec in sections.values():
        if sec["rva"] <= rva < sec["rva"] + sec["vsize"]:
            return sec["offset"] + (rva - sec["rva"])
    return None


def raw_to_va(offset, image_base, sections):
    for sec in sections.values():
        if sec["offset"] <= offset < sec["offset"] + sec["size"]:
            rva = sec["rva"] + (offset - sec["offset"])
            return image_base + rva
    return None


def read_u32(raw, offset):
    if offset + 4 > len(raw):
        return None
    return struct.unpack_from("<I", raw, offset)[0]


def is_code_va(va, image_base, sections):
    if ".text" not in sections:
        return False
    sec = sections[".text"]
    rva = va - image_base
    return sec["rva"] <= rva < sec["rva"] + sec["vsize"]


def scan_rdata_for_strings(raw, image_base, sections):
    """Scan .rdata for TARGET_STRINGS and collect (string, rdata_offset, va) tuples."""
    if ".rdata" not in sections:
        print("[WARN] .rdata section not found")
        return []

    sec = sections[".rdata"]
    rdata_start = sec["offset"]
    rdata_end   = rdata_start + sec["size"]
    rdata_rva   = sec["rva"]

    results = []
    for target in TARGET_STRINGS:
        search = target + b"\x00"  # null-terminated
        pos = rdata_start
        while True:
            idx = raw.find(search, pos, rdata_end)
            if idx == -1:
                break
            va = image_base + rdata_rva + (idx - rdata_start)
            results.append((target.decode("ascii"), idx, va))
            pos = idx + 1

    return results


def find_func_pointers(raw, image_base, sections, string_entries):
    """
    For each found string, search the ENTIRE file for a luaL_Reg pattern:
      [string_va: 4 bytes][func_va: 4 bytes]
    where func_va points into .text. Table may be in .rdata or .data.
    Returns dict: func_name -> (func_va, string_va)
    """
    found = {}
    str_va_map = {va: name for (name, _, va) in string_entries}

    # Scan entire PE file at 4-byte alignment
    for off in range(0, len(raw) - 8, 4):
        val = read_u32(raw, off)
        if val in str_va_map:
            func_va = read_u32(raw, off + 4)
            if func_va and is_code_va(func_va, image_base, sections):
                name = str_va_map[val]
                if name not in found:
                    found[name] = (func_va, val)
                    loc_va = raw_to_va(off, image_base, sections)
                    loc_str = f"0x{loc_va:08X}" if loc_va else f"raw+0x{off:X}"
                    print(f"  Found: {name} -> func @ 0x{func_va:08X}  (table entry @ {loc_str})")

    return found


def disassemble(md, raw, va, image_base, sections, max_size=0x200):
    raw_off = va_to_raw(va, image_base, sections)
    if raw_off is None:
        return []
    code = raw[raw_off: raw_off + max_size]
    lines = []
    for insn in md.disasm(code, va):
        line = f"  0x{insn.address:08X}:  {insn.mnemonic:<8s} {insn.op_str}"
        lines.append((insn.address, insn.mnemonic, insn.op_str, line))
        if insn.mnemonic in ("ret", "retn"):
            break
        if insn.mnemonic == "int3":
            break
        if insn.address - va > max_size:
            break
    return lines


def extract_globals(lines, image_base):
    """Extract references to .data/.bss global addresses from disassembly lines."""
    refs = []
    for (addr, mnem, ops, line) in lines:
        # direct memory: [0x00xxxxxx]
        for m in re.findall(r"\[0x([0-9a-f]{6,8})\]", ops, re.I):
            gva = int(m, 16)
            if 0x00800000 <= gva <= 0x01200000:
                refs.append((gva, line))
        # immediate: mov/cmp/push/add/lea ..., 0x00xxxxxx
        for m in re.findall(r"(?:mov|lea|push|cmp|add|sub|imul|inc|dec).*?(?:\s|,)\s*0x([0-9a-f]{6,8})", ops, re.I):
            gva = int(m, 16)
            if 0x00800000 <= gva <= 0x01200000:
                refs.append((gva, line))
    return refs


def main():
    print(f"Loading {WOW_EXE}...")
    raw, image_base, sections = load_sections(WOW_EXE)
    print(f"Image base: 0x{image_base:08X}")
    for name, sec in sections.items():
        print(f"  {name}: RVA=0x{sec['rva']:08X}, VSize=0x{sec['vsize']:08X}")

    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    md.detail = False

    print("\n--- Scanning .rdata for quest log strings ---")
    string_entries = scan_rdata_for_strings(raw, image_base, sections)
    print(f"Found {len(string_entries)} string occurrences:")
    for name, off, va in string_entries:
        print(f"  '{name}' @ raw=0x{off:08X} va=0x{va:08X}")

    print("\n--- Finding luaL_Reg function pointers ---")
    func_map = find_func_pointers(raw, image_base, sections, string_entries)

    if not func_map:
        print("[ERROR] No function pointers found. Check .rdata scan range.")
        return

    print(f"\n--- Disassembling {len(func_map)} functions ---")
    all_globals = {}  # gva -> [(func_name, line)]

    # Also disassemble inner helpers referenced by GetQuestLogTitle
    INNER_HELPERS = {
        "inner_GetTitle":      0x005E0000,
        "inner_GetTag":        0x005E0070,
        "inner_GetIsComplete": 0x005DED30,
        "inner_GetLevel":      0x005E0D20,
        "inner_GetSlotIdx":    0x005DEBD0,
    }
    FUNC_SIZES = {
        "GetQuestLogTitle": 0x400,  # long function, need more bytes
    }
    for hname, hva in INNER_HELPERS.items():
        func_map[hname] = (hva, 0)

    priority = [
        "GetNumQuestLogEntries",
        "GetQuestLogTitle",
        "inner_GetTitle",
        "inner_GetLevel",
        "inner_GetIsComplete",
        "inner_GetSlotIdx",
        "SelectQuestLogEntry",
        "AbandonQuest",
        "IsCurrentQuestFailed",
        "IsQuestWatched",
    ]
    ordered = [(n, func_map[n]) for n in priority if n in func_map]
    ordered += [(n, v) for n, v in func_map.items() if n not in priority]

    for func_name, (func_va, _) in ordered:
        max_sz = FUNC_SIZES.get(func_name, 0x180)
        print(f"\n{'='*70}")
        print(f"  {func_name} @ 0x{func_va:08X}")
        print(f"{'='*70}")
        lines = disassemble(md, raw, func_va, image_base, sections, max_sz)
        for (_, _, _, txt) in lines:
            print(txt)

        refs = extract_globals(lines, image_base)
        for gva, ctx in refs:
            if gva not in all_globals:
                all_globals[gva] = []
            all_globals[gva].append((func_name, ctx))

    print(f"\n{'='*70}")
    print("  GLOBAL/STATIC ADDRESSES REFERENCED BY QUEST LOG FUNCTIONS")
    print(f"{'='*70}")
    for gva in sorted(all_globals.keys()):
        refs = all_globals[gva]
        print(f"\n  0x{gva:08X}  (referenced by {len(refs)} place(s)):")
        for fname, ctx in refs:
            print(f"    [{fname}]  {ctx.strip()}")


if __name__ == "__main__":
    main()
