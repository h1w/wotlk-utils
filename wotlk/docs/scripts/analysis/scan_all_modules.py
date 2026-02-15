"""
Scan ALL Warden modules for dispatcher patterns.
Decompresses decrypted files that don't have _decompressed versions yet.
Searches for:
1. Remap table + jump table (response builder)
2. Binary-search cmp/je chains (request parser - the real check type dispatcher)
3. XOR byte unmasking patterns
"""

import struct
import zlib
import os
import glob
from capstone import *

DUMP_DIR = r"Z:\Games\wow 3.3.5a client\warden_dumps"
DOCS_DIR = r"C:\Users\bpqvg\source\repos\wotlk-utils\wotlk\docs\warden_dumps"

def decompress_module(decrypted_data):
    """Decompress: [uint32 decompressed_size][zlib_data]"""
    if len(decrypted_data) < 5:
        return None
    decomp_size = struct.unpack_from('<I', decrypted_data, 0)[0]
    if decomp_size == 0 or decomp_size > 4*1024*1024:
        return None
    if decrypted_data[4] != 0x78:  # zlib magic
        return None
    try:
        result = zlib.decompress(decrypted_data[4:])
        return result
    except zlib.error:
        return None

def load_module(hash_str, dirs):
    """Try to load decompressed module, or decompress from decrypted."""
    for d in dirs:
        # Try decompressed first
        path = os.path.join(d, f"warden_{hash_str}_decompressed.bin")
        if os.path.exists(path):
            with open(path, 'rb') as f:
                return f.read(), "decompressed"
        # Try decrypted
        path = os.path.join(d, f"warden_{hash_str}_decrypted.bin")
        if os.path.exists(path):
            with open(path, 'rb') as f:
                decrypted = f.read()
            data = decompress_module(decrypted)
            if data:
                # Save decompressed for next time
                decomp_path = os.path.join(d, f"warden_{hash_str}_decompressed.bin")
                with open(decomp_path, 'wb') as f:
                    f.write(data)
                return data, "decompressed from decrypted"
            return None, "decompress failed"
    return None, "not found"

def parse_header(data):
    if len(data) < 0x28:
        return None
    fields = struct.unpack_from('<10I', data, 0)
    names = ['moduleSize', 'reserved', 'relocDataOff', 'relocCount',
             'exportTableOff', 'exportCount', 'baseIndex',
             'importTableOff', 'importLibCount', 'sectionDescCount']
    h = dict(zip(names, fields))
    h['packedDataOff'] = 0x28 + h['sectionDescCount'] * 12
    return h

def scan_remap_pattern(data, scan_start):
    """Find movzx r32, byte ptr [r32 + disp32] + jmp [r32*4 + disp32]."""
    for i in range(scan_start, min(len(data) - 7, len(data))):
        if data[i] != 0x0F or data[i+1] != 0xB6:
            continue
        modrm = data[i+2]
        if not (0x80 <= modrm <= 0xBF) or (modrm & 7) == 4:
            continue
        remap_off = struct.unpack_from('<I', data, i+3)[0]
        if remap_off < 0x100 or remap_off + 256 > len(data):
            continue
        for j in range(i+7, min(i+57, len(data) - 7)):
            if data[j] != 0xFF or data[j+1] != 0x24:
                continue
            sib = data[j+2]
            if (sib >> 6) != 2 or (sib & 7) != 5:
                continue
            jtable_off = struct.unpack_from('<I', data, j+3)[0]
            # Find max_type from preceding cmp
            max_type = 0xFF
            for k in range(i-1, max(scan_start, i-40), -1):
                if k + 5 <= len(data) and data[k] == 0x3D:
                    val = struct.unpack_from('<I', data, k+1)[0]
                    if 0x80 <= val <= 0xFF:
                        max_type = val
                        break
                if k + 2 <= len(data) and data[k] == 0x3C:
                    val = data[k+1]
                    if 0x80 <= val <= 0xFF:
                        max_type = val
                        break
            return remap_off, jtable_off, i, j, max_type
    return None

def scan_xor_cmp_dispatch(data, scan_start):
    """
    Find XOR + movzx + binary-search cmp/je chain.
    This is the actual request parser.
    Returns list of (xor_offset, type_values_found).
    """
    md = Cs(CS_ARCH_X86, CS_MODE_32)
    md.detail = True
    code = data[scan_start:]
    insns = list(md.disasm(code, scan_start))

    results = []
    for i in range(len(insns) - 10):
        insn = insns[i]
        # Look for: xor r8, byte ptr [reg + small_offset]
        if insn.mnemonic != 'xor':
            continue
        if len(insn.operands) != 2:
            continue
        dst, src = insn.operands
        if not (dst.type == CS_OP_REG and src.type == CS_OP_MEM and src.size == 1):
            continue

        # Check for movzx within next 5 instructions
        movzx_idx = None
        for j in range(i+1, min(i+6, len(insns))):
            if insns[j].mnemonic == 'movzx':
                movzx_idx = j
                break
        if movzx_idx is None:
            continue

        # Look for cmp/je chains after movzx (up to 40 instructions)
        type_values = []
        for j in range(movzx_idx+1, min(movzx_idx+40, len(insns))):
            ni = insns[j]
            if ni.mnemonic in ('cmp', 'sub') and ni.op_count(CS_OP_IMM) > 0:
                for op in ni.operands:
                    if op.type == CS_OP_IMM:
                        val = op.imm & 0xFFFFFFFF
                        if 0 < val <= 0xFF:
                            type_values.append(val)

        if len(type_values) >= 3:  # At least 3 type comparisons = likely dispatcher
            results.append((insn.address, type_values))

    return results

def extract_handler_groups(data, remap_off, max_type):
    """Read remap table, group by handler."""
    table = data[remap_off:remap_off + max_type + 1]
    counts = {}
    for i in range(len(table)):
        idx = table[i]
        counts[idx] = counts.get(idx, 0) + 1
    default_idx = max(counts.keys(), key=lambda k: counts[k])
    groups = {}
    for i in range(len(table)):
        idx = table[i]
        if idx != default_idx:
            if idx not in groups:
                groups[idx] = []
            groups[idx].append(i)
    return groups, default_idx

def analyze_module(hash_str, data, source):
    header = parse_header(data)
    if not header:
        print(f"  ERROR: Invalid header")
        return

    print(f"  Size: {len(data)} bytes, runtime: {header['moduleSize']}, "
          f"sections: {header['sectionDescCount']}, source: {source}")

    scan_start = header['packedDataOff']

    # 1. Scan for remap+jump table
    remap_result = scan_remap_pattern(data, scan_start)
    if remap_result:
        remap_off, jtable_off, movzx_pos, jmp_pos, max_type = remap_result
        groups, default_idx = extract_handler_groups(data, remap_off, max_type)
        print(f"  REMAP TABLE: at 0x{remap_off:04x}, jtable=0x{jtable_off:04x}, "
              f"max_type=0x{max_type:02x}, {len(groups)} groups, default={default_idx}")
    else:
        print(f"  REMAP TABLE: NOT FOUND")

    # 2. Scan for XOR + cmp/je binary-search dispatcher
    xor_results = scan_xor_cmp_dispatch(data, scan_start)
    if xor_results:
        for xor_off, types in xor_results:
            print(f"  CMP DISPATCH: at 0x{xor_off:04x}, {len(types)} comparisons: "
                  f"{[f'0x{t:02x}' for t in types]}")
    else:
        print(f"  CMP DISPATCH: NOT FOUND")

def main():
    # Collect all unique module hashes from both directories
    dirs = [DUMP_DIR, DOCS_DIR]
    hashes = set()
    for d in dirs:
        if not os.path.exists(d):
            continue
        for f in os.listdir(d):
            if f.startswith('warden_') and ('_decompressed' in f or '_decrypted' in f):
                # Extract hash: warden_HASH_type.bin
                parts = f.split('_')
                if len(parts) >= 3:
                    h = parts[1]
                    if len(h) == 32:  # MD5 hash length
                        hashes.add(h)

    print(f"Found {len(hashes)} unique module hashes\n")

    for h in sorted(hashes):
        print(f"\n{'='*70}")
        print(f"Module: {h}")
        print(f"{'='*70}")

        data, source = load_module(h, dirs)
        if data is None:
            print(f"  SKIP: {source}")
            continue

        analyze_module(h, data, source)

if __name__ == '__main__':
    main()
