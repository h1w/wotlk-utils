"""Verify 1A615549 dispatch chain: 7 types found, all others have 9-10.
Check remap table for possible missed types."""

import struct, os
from capstone import *
from capstone.x86 import *

DOCS_DIR = r"C:\Users\bpqvg\source\repos\wotlk-utils\wotlk\docs\warden_dumps"
md = Cs(CS_ARCH_X86, CS_MODE_32)
md.detail = True


def load_module(hash_str):
    path = os.path.join(DOCS_DIR, f"warden_{hash_str}_decompressed.bin")
    with open(path, 'rb') as f:
        return f.read()


data = load_module("1A615549A9CC44C4B3B3289864791D8F")
print(f"Size: {len(data)} bytes")

# Dispatch chain at XOR @ 0x34f7
print("\n=== Dispatch chain disassembly (XOR @ 0x34f7) ===")
end = min(0x34f7 + 200 * 15, len(data))
insns = list(md.disasm(data[0x34f7:end], 0x34f7))

for insn in insns[:60]:
    raw = ' '.join(f'{b:02x}' for b in insn.bytes)
    marker = ""
    if insn.mnemonic == 'xor':
        marker = " <<<"
    elif insn.mnemonic == 'movzx':
        marker = " <<<"
    elif insn.mnemonic in ('cmp', 'test', 'sub', 'dec'):
        marker = " <<<"
    elif insn.mnemonic in ('je', 'jne', 'jg', 'jge', 'jl', 'jle', 'ja', 'jae', 'jb', 'jbe', 'jmp'):
        marker = " <<<"
    elif insn.mnemonic in ('push', 'call', 'ret', 'leave'):
        marker = " *** STOP ***"
    print(f"  0x{insn.address:04x}: {raw:30s} {insn.mnemonic:8s} {insn.op_str}{marker}")

# Check the jle target (lower branch)
print("\n\n=== Lower branch (should be at what address after cmp+jg+je) ===")
# The dispatch chain trace shows:
# [root] cmp 0xae + jg + je -> TYPE 0xae, follow jg @ 0x38dd
# After that: sub 0x00 + je -> TYPE 0x00; sub 0x11 + jne -> TYPE 0x11 (last)
# The fall-through after je should have more dispatching

# Show remap table analysis
print("\n=== Remap table @ 0x5ec4 (shift=0x11, max=0xe8) ===")
remap_off = 0x5ec4
shift = 0x11
max_type = 0xe8
table = data[remap_off:remap_off + max_type + 1]

index_counts = {}
for idx in table:
    index_counts[idx] = index_counts.get(idx, 0) + 1

default_idx = max(index_counts.keys(), key=lambda k: index_counts[k])

groups = {}
for i in range(len(table)):
    idx = table[i]
    if idx != default_idx:
        if idx not in groups:
            groups[idx] = []
        groups[idx].append(i)

singletons = []
pairs = []
for idx, members in sorted(groups.items(), key=lambda x: len(x[1])):
    real_members = [(m + shift) & 0xFF for m in members]
    if len(members) == 1:
        singletons.append(real_members[0])
    elif len(members) == 2:
        pairs.append(real_members)

dispatch_types = {0x00, 0x11, 0xAE, 0xBF, 0xD7, 0xE8, 0xF9}
print(f"Default idx: {default_idx}, {index_counts[default_idx]}/{max_type+1} entries")
print(f"Singletons ({len(singletons)}): {[f'0x{t:02x}' for t in sorted(singletons)]}")
print(f"Pairs ({len(pairs)}): {pairs}")
print(f"\nDispatch types: {[f'0x{t:02x}' for t in sorted(dispatch_types)]}")

# Check which dispatch types are singletons in remap
in_remap = dispatch_types & set(singletons)
not_in_remap = dispatch_types - set(singletons)
print(f"Dispatch types in remap singletons: {[f'0x{t:02x}' for t in sorted(in_remap)]}")
print(f"Dispatch types NOT in remap singletons: {[f'0x{t:02x}' for t in sorted(not_in_remap)]}")

# Check remap singletons that are NOT in dispatch types
extra_remap = set(singletons) - dispatch_types
print(f"Remap singletons NOT in dispatch: {[f'0x{t:02x}' for t in sorted(extra_remap)]}")
