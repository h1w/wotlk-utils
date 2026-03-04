"""
Extended disassembly — see the 'found' paths for GetAvailableQuestInfoFromIndex
and GetActiveQuestFromIndex (which are past the inline ret).
Also disassemble ClearGossipQuests and SelectGossipActiveQuest at their true addresses.
"""

import pefile
import capstone

WOW_EXE = r"Z:\Games\wow 3.3.5a client\Wow.exe"

# Extended sizes to capture found path past the inline ret
TARGETS = {
    "GetAvailableQuestInfoFromIndex_FULL": (0x0058A660, 0xC0),
    "GetActiveQuestFromIndex_FULL":        (0x0058A750, 0xC0),
    "ClearGossipQuests":                   (0x0058A7B0, 0x80),
    # Disassemble the actual SelectGossipActiveQuest (the lua_ version at 0x58B670
    # delegates, let's find the real one)
    "lua_SelectGossipActiveQuest_FULL":    (0x0058B670, 0x120),
    # Function called by lua_GetGossipActiveQuests for questId lookup
    "QuestLogLookup_5deeb0":               (0x005DEEB0, 0x80),
    # Look at what's between quest entries end and gossip options
    "GossipStructHeader_area":             (0x0058A840, 0x40), # SMSG_GOSSIP_COMPLETE
}


def main():
    pe = pefile.PE(WOW_EXE, fast_load=True)
    image_base = pe.OPTIONAL_HEADER.ImageBase
    text_sec = None
    for s in pe.sections:
        if s.Name.rstrip(b'\x00') == b'.text':
            text_sec = s
            break

    with open(WOW_EXE, 'rb') as f:
        data = f.read()

    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    md.detail = False

    for name, (va, sz) in TARGETS.items():
        rva = va - image_base
        for sec in pe.sections:
            sr = sec.VirtualAddress
            se = sr + sec.Misc_VirtualSize
            if sr <= rva < se:
                off = sec.PointerToRawData + (rva - sr)
                code = data[off:off+sz]
                break
        else:
            print(f"[SKIP] {name} @ 0x{va:08X}")
            continue

        print(f"\n{'='*70}")
        print(f"  {name} @ 0x{va:08X}")
        print(f"{'='*70}")
        ret_count = 0
        for insn in md.disasm(code, va):
            print(f"  0x{insn.address:08X}:  {insn.mnemonic:<8s} {insn.op_str}")
            if insn.mnemonic == 'ret':
                ret_count += 1
                if ret_count >= 2:
                    break
            if insn.mnemonic == 'int3':
                break


if __name__ == "__main__":
    main()
