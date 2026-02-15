"""
Find the RC4 PRGA function in all Warden module binaries.

Strategy:
1. Raw byte scan for MOVZX with displacements 0x100/0x101
   (i/j offsets in [S[256]][i][j] RC4 context layout)
2. Also scan for alternative [i][j][S[256]] layout (small disp + SIB with disp8=2)
3. Group nearby matches to identify RC4 PRGA function region
4. Disassemble from function prologue using Capstone
5. Extract byte signature and calling convention

Validates across all available decompressed module binaries.
"""

import struct
import zlib
import os
import sys
from collections import defaultdict

try:
    from capstone import *
except ImportError:
    print("ERROR: pip install capstone")
    sys.exit(1)

DOCS_DIR = os.path.join(os.path.dirname(__file__), "..", "warden_dumps")

# All known module hashes (from disk dumps)
MODULE_HASHES = []


def decompress_module(decrypted_data):
    if len(decrypted_data) < 5:
        return None
    decomp_size = struct.unpack_from('<I', decrypted_data, 0)[0]
    if decomp_size == 0 or decomp_size > 4 * 1024 * 1024:
        return None
    if decrypted_data[4] != 0x78:
        return None
    try:
        return zlib.decompress(decrypted_data[4:])
    except zlib.error:
        return None


def load_module(hash_str):
    """Load decompressed module binary from disk."""
    # Try decompressed first
    path = os.path.join(DOCS_DIR, f"warden_{hash_str}_decompressed.bin")
    if os.path.exists(path):
        with open(path, 'rb') as f:
            return f.read()
    # Try decrypted (needs decompression)
    path = os.path.join(DOCS_DIR, f"warden_{hash_str}_decrypted.bin")
    if os.path.exists(path):
        with open(path, 'rb') as f:
            return decompress_module(f.read())
    return None


def discover_modules():
    """Find all module hashes in the warden_dumps directory."""
    hashes = set()
    if not os.path.isdir(DOCS_DIR):
        print(f"ERROR: Dump directory not found: {DOCS_DIR}")
        return []
    for fname in os.listdir(DOCS_DIR):
        if fname.startswith("warden_") and (fname.endswith("_decompressed.bin") or fname.endswith("_decrypted.bin")):
            # Extract hash: warden_HASH_suffix.bin
            parts = fname.split('_')
            if len(parts) >= 3:
                h = parts[1]
                if len(h) == 32:  # MD5 hash
                    hashes.add(h)
    return sorted(hashes)


def get_packed_data_offset(data):
    """Get offset where packed section data starts in the module binary.
    Header: 40 bytes (0x28)
    Section descriptors: 12 bytes each, count at header[0x24]
    Packed data: immediately after section descriptors
    """
    if len(data) < 0x28:
        return None
    section_desc_count = struct.unpack_from('<I', data, 0x24)[0]
    offset = 0x28 + section_desc_count * 12
    if offset >= len(data):
        return None
    return offset


def scan_movzx_disp32(data, start_offset=0):
    """Scan for MOVZX r32, byte ptr [reg+disp32] with disp32 = 0x100 or 0x101.
    Encoding: 0F B6 [ModRM: mod=10, reg=any, rm!=4] [disp32]
    With SIB (rm=4): 0F B6 [ModRM: mod=10, rm=4] [SIB] [disp32]
    """
    results = []  # (offset, displacement, instruction_bytes)

    for i in range(start_offset, len(data) - 6):
        if data[i] != 0x0F or data[i + 1] != 0xB6:
            continue

        modrm = data[i + 2]
        mod = (modrm >> 6) & 3
        rm = modrm & 7

        if mod != 2:  # mod=10 = 32-bit displacement
            continue

        if rm == 4:
            # SIB follows, then 4-byte displacement
            if i + 8 > len(data):
                continue
            disp = struct.unpack_from('<I', data, i + 4)[0]
            instr_len = 8
        else:
            # No SIB, 4-byte displacement follows ModRM
            if i + 7 > len(data):
                continue
            disp = struct.unpack_from('<I', data, i + 3)[0]
            instr_len = 7

        if disp in (0x100, 0x101):
            results.append((i, disp, data[i:i + instr_len]))

    return results


def scan_mov_byte_disp32(data, start_offset=0):
    """Scan for MOV byte ptr [reg+disp32], r8 with disp32 = 0x100 or 0x101.
    Encoding: 88 [ModRM: mod=10, reg=any, rm!=4] [disp32]
    Also: ADD byte ptr [reg+disp32], r8: 00 [ModRM] [disp32]
    """
    results = []

    for opcode in (0x88, 0x00, 0x80, 0xFE):
        for i in range(start_offset, len(data) - 6):
            if data[i] != opcode:
                continue

            modrm = data[i + 1]
            mod = (modrm >> 6) & 3
            rm = modrm & 7

            if mod != 2:
                continue

            if rm == 4:
                if i + 7 > len(data):
                    continue
                disp = struct.unpack_from('<I', data, i + 3)[0]
                instr_len = 7 + (1 if opcode == 0x80 else 0)
            else:
                if i + 6 > len(data):
                    continue
                disp = struct.unpack_from('<I', data, i + 2)[0]
                instr_len = 6 + (1 if opcode == 0x80 else 0)

            if disp in (0x100, 0x101):
                results.append((i, disp, data[i:i + instr_len]))

    return results


def cluster_matches(matches, window=120):
    """Group matches that are within `window` bytes of each other."""
    if not matches:
        return []

    sorted_m = sorted(matches, key=lambda x: x[0])
    clusters = []
    current = [sorted_m[0]]

    for m in sorted_m[1:]:
        if m[0] - current[0][0] <= window:
            current.append(m)
        else:
            clusters.append(current)
            current = [m]
    clusters.append(current)
    return clusters


def find_function_prologue(data, offset, max_backtrack=256):
    """Walk backward from offset to find function prologue.
    Looks for: push ebp (55), int3 padding (CC), or other prologue patterns.
    Returns offset of function start.
    """
    best = offset

    for back in range(1, min(max_backtrack, offset) + 1):
        pos = offset - back
        b = data[pos]

        # push ebp; mov ebp, esp (55 8B EC)
        if pos + 2 < len(data) and data[pos] == 0x55 and data[pos + 1] == 0x8B and data[pos + 2] == 0xEC:
            # Check if preceded by padding (CC/90/C3) or start of file
            if pos == 0 or data[pos - 1] in (0xCC, 0x90, 0xC3):
                return pos
            best = pos

        # INT3 (CC) padding before function
        if b == 0xCC:
            # Next byte should be the function start (push or mov)
            next_pos = pos + 1
            if next_pos < len(data) and data[next_pos] in (0x55, 0x53, 0x56, 0x57, 0x51, 0x52):
                return next_pos
            break  # Stop at INT3 boundary

        # RET (C3/C2) of previous function
        if b == 0xC3 or b == 0xC2:
            return pos + 1 if b == 0xC3 else pos + 3

    return best


def analyze_function(data, func_start, max_len=512):
    """Disassemble function using Capstone and extract calling convention info."""
    md = Cs(CS_ARCH_X86, CS_MODE_32)
    md.detail = True

    func_data = data[func_start:func_start + max_len]
    instructions = list(md.disasm(func_data, func_start))

    if not instructions:
        return None

    info = {
        'start': func_start,
        'prologue': [],
        'ret_type': None,
        'uses_ecx': False,
        'stack_frame': False,
        'instructions': instructions,
        'total_movzx_100': 0,
        'total_movzx_101': 0,
        'total_mov_store_100': 0,
        'total_mov_store_101': 0,
    }

    # Analyze prologue
    for insn in instructions[:8]:
        info['prologue'].append(f"{insn.mnemonic} {insn.op_str}")
        if insn.mnemonic == 'push' and insn.op_str == 'ebp':
            info['stack_frame'] = True
        if insn.mnemonic == 'mov' and insn.op_str == 'ebp, esp':
            info['stack_frame'] = True

    # Count references to 0x100/0x101
    for insn in instructions:
        if insn.mnemonic == 'ret':
            if insn.op_str:
                info['ret_type'] = f"ret {insn.op_str}"
            else:
                info['ret_type'] = "ret"
            break

        # Check for ECX usage (thiscall indicator)
        if 'ecx' in insn.op_str and insn.mnemonic in ('mov', 'push', 'lea'):
            info['uses_ecx'] = True

        # Check for MOVZX/MOV with 0x100/0x101 displacement
        op_str = insn.op_str
        if '0x100' in op_str or '0x101' in op_str:
            if insn.mnemonic == 'movzx':
                if '0x100' in op_str:
                    info['total_movzx_100'] += 1
                if '0x101' in op_str:
                    info['total_movzx_101'] += 1
            elif insn.mnemonic == 'mov':
                if '0x100' in op_str:
                    info['total_mov_store_100'] += 1
                if '0x101' in op_str:
                    info['total_mov_store_101'] += 1

    return info


def extract_signature(data, func_start, sig_len=32):
    """Extract byte signature from function start."""
    end = min(func_start + sig_len, len(data))
    return data[func_start:end]


def format_signature(sig_bytes):
    """Format byte signature as C-style hex array."""
    parts = []
    for b in sig_bytes:
        parts.append(f"0x{b:02X}")
    return ", ".join(parts)


def disasm_range(data, start, length):
    """Disassemble a range and return formatted output."""
    md = Cs(CS_ARCH_X86, CS_MODE_32)
    md.detail = True
    lines = []
    for insn in md.disasm(data[start:start + length], start):
        hex_bytes = ' '.join(f'{b:02X}' for b in insn.bytes)
        lines.append(f"  0x{insn.address:04X}: {hex_bytes:30s} {insn.mnemonic} {insn.op_str}")
    return '\n'.join(lines)


def analyze_module(hash_str, data):
    """Analyze a single module binary for RC4 PRGA function."""
    packed_offset = get_packed_data_offset(data)
    if packed_offset is None:
        return None

    module_size = struct.unpack_from('<I', data, 0x00)[0]

    print(f"\n{'='*70}")
    print(f"Module: {hash_str[:8]}...")
    print(f"  Size: {len(data)} bytes, moduleSize field: {module_size}")
    print(f"  Packed data offset: 0x{packed_offset:X}")

    # Scan for MOVZX with 0x100/0x101 displacements
    movzx_matches = scan_movzx_disp32(data, packed_offset)
    mov_matches = scan_mov_byte_disp32(data, packed_offset)

    all_matches = movzx_matches + mov_matches

    if not all_matches:
        print("  NO matches for 0x100/0x101 displacement patterns")
        # Try scanning entire binary (not just packed data)
        movzx_matches = scan_movzx_disp32(data, 0)
        mov_matches = scan_mov_byte_disp32(data, 0)
        all_matches = movzx_matches + mov_matches
        if all_matches:
            print(f"  Found {len(all_matches)} matches in full binary scan")
        else:
            print("  NO matches in full binary either")
            return None

    print(f"  Found {len(movzx_matches)} MOVZX + {len(mov_matches)} MOV/ADD/etc matches")
    for off, disp, instr_bytes in all_matches:
        hex_b = ' '.join(f'{b:02X}' for b in instr_bytes)
        print(f"    offset=0x{off:04X} disp=0x{disp:03X} bytes=[{hex_b}]")

    # Cluster nearby matches
    clusters = cluster_matches(all_matches, window=120)
    print(f"  {len(clusters)} cluster(s) found")

    best_cluster = None
    best_score = 0

    for ci, cluster in enumerate(clusters):
        disps = set(m[1] for m in cluster)
        has_100 = 0x100 in disps
        has_101 = 0x101 in disps
        score = len(cluster) * (2 if (has_100 and has_101) else 1)

        print(f"    Cluster {ci}: {len(cluster)} matches, "
              f"offsets 0x{cluster[0][0]:04X}-0x{cluster[-1][0]:04X}, "
              f"disps={sorted(hex(d) for d in disps)}, score={score}")

        if score > best_score:
            best_score = score
            best_cluster = cluster

    if not best_cluster or best_score < 2:
        print("  No strong RC4 cluster found")
        return None

    # Find function start
    first_match_offset = best_cluster[0][0]
    func_start = find_function_prologue(data, first_match_offset)
    func_end = best_cluster[-1][0] + len(best_cluster[-1][2])

    print(f"\n  RC4 function region: 0x{func_start:04X} - 0x{func_end:04X}")
    print(f"  Function prologue found at: 0x{func_start:04X}")

    # Disassemble the function
    func_len = min(func_end - func_start + 128, 512)  # extra bytes after last match
    print(f"\n  Disassembly (0x{func_start:04X}, {func_len} bytes):")
    print(disasm_range(data, func_start, func_len))

    # Analyze calling convention
    info = analyze_function(data, func_start, func_len)
    if info:
        print(f"\n  Prologue: {' | '.join(info['prologue'][:6])}")
        print(f"  Stack frame: {info['stack_frame']}")
        print(f"  Return: {info['ret_type']}")
        print(f"  Uses ECX: {info['uses_ecx']}")
        print(f"  MOVZX 0x100 count: {info['total_movzx_100']}")
        print(f"  MOVZX 0x101 count: {info['total_movzx_101']}")
        print(f"  MOV store 0x100 count: {info['total_mov_store_100']}")
        print(f"  MOV store 0x101 count: {info['total_mov_store_101']}")

    # Extract signature
    sig = extract_signature(data, func_start, 32)
    print(f"\n  Byte signature ({len(sig)} bytes from 0x{func_start:04X}):")
    print(f"    {format_signature(sig)}")
    print(f"    hex: {sig.hex()}")

    return {
        'hash': hash_str,
        'func_start': func_start,
        'func_region_end': func_end,
        'signature': sig,
        'info': info,
        'cluster': best_cluster,
    }


def compare_signatures(results):
    """Compare signatures across all modules to find stable bytes."""
    if len(results) < 2:
        return

    print(f"\n{'='*70}")
    print("CROSS-MODULE COMPARISON")
    print(f"{'='*70}")

    min_len = min(len(r['signature']) for r in results)
    sigs = [r['signature'][:min_len] for r in results]

    # Find bytes that are identical across all modules
    stable_mask = []
    stable_bytes = []
    for i in range(min_len):
        vals = set(s[i] for s in sigs)
        if len(vals) == 1:
            stable_mask.append(True)
            stable_bytes.append(sigs[0][i])
        else:
            stable_mask.append(False)
            stable_bytes.append(None)

    print(f"\nStable bytes across {len(results)} modules (first {min_len} bytes):")
    line = []
    for i in range(min_len):
        if stable_mask[i]:
            line.append(f"{sigs[0][i]:02X}")
        else:
            vals = sorted(set(s[i] for s in sigs))
            line.append(f"??({'/'.join(f'{v:02X}' for v in vals)})")
    print("  " + " ".join(line))

    # Find longest stable prefix
    prefix_len = 0
    for i in range(min_len):
        if stable_mask[i]:
            prefix_len = i + 1
        else:
            break

    if prefix_len > 0:
        print(f"\nStable prefix: {prefix_len} bytes")
        print(f"  {format_signature(sigs[0][:prefix_len])}")
    else:
        print("\nWARNING: No stable prefix — function prologues differ across modules")

    # Find all contiguous stable runs
    runs = []
    run_start = None
    for i in range(min_len):
        if stable_mask[i]:
            if run_start is None:
                run_start = i
        else:
            if run_start is not None:
                runs.append((run_start, i))
                run_start = None
    if run_start is not None:
        runs.append((run_start, min_len))

    print(f"\nAll stable runs (>= 4 bytes):")
    for start, end in runs:
        length = end - start
        if length >= 4:
            print(f"  offset +{start}: {length} bytes: {format_signature(sigs[0][start:end])}")

    # Calling convention summary
    print(f"\nCalling convention summary:")
    for r in results:
        info = r['info']
        if info:
            ret = info['ret_type'] or 'unknown'
            frame = 'frame' if info['stack_frame'] else 'no-frame'
            ecx = 'uses-ecx' if info['uses_ecx'] else 'no-ecx'
            print(f"  {r['hash'][:8]}: {ret}, {frame}, {ecx}")

    # Return instruction analysis for __stdcall vs __cdecl
    ret_types = set()
    for r in results:
        if r['info'] and r['info']['ret_type']:
            ret_types.add(r['info']['ret_type'])
    print(f"\n  Return types seen: {ret_types}")
    if len(ret_types) == 1:
        ret = list(ret_types)[0]
        if 'ret' == ret:
            print("  -> Likely __cdecl or __thiscall (caller cleans stack)")
        elif ret.startswith('ret 0x') or ret.startswith('ret '):
            print(f"  -> Likely __stdcall (callee cleans {ret})")


def main():
    hashes = discover_modules()
    if not hashes:
        print("No modules found. Check DOCS_DIR path.")
        return

    print(f"Found {len(hashes)} module(s) in {DOCS_DIR}")

    results = []
    failed = []

    for h in hashes:
        data = load_module(h)
        if data is None:
            print(f"\n  {h[:8]}: could not load")
            failed.append(h)
            continue

        result = analyze_module(h, data)
        if result:
            results.append(result)
        else:
            failed.append(h)

    # Summary
    print(f"\n{'='*70}")
    print(f"SUMMARY: {len(results)}/{len(hashes)} modules analyzed successfully")
    if failed:
        print(f"  Failed: {', '.join(h[:8] for h in failed)}")

    if len(results) >= 2:
        compare_signatures(results)

    # Output C++ signature for use in warden_rc4_hook.cpp
    if results:
        print(f"\n{'='*70}")
        print("C++ SIGNATURE (for warden_rc4_hook.cpp):")
        print("// Byte signature from Python analysis — update after running find_rc4_function.py")

        # Use the first module's signature if all are identical, or stable bytes
        if len(results) >= 2:
            min_len = min(len(r['signature']) for r in results)
            sigs = [r['signature'][:min_len] for r in results]
            # Find longest stable prefix
            prefix_len = 0
            for i in range(min_len):
                vals = set(s[i] for s in sigs)
                if len(vals) == 1:
                    prefix_len = i + 1
                else:
                    break
            if prefix_len >= 6:
                sig = sigs[0][:prefix_len]
                print(f"static constexpr uint8_t kRC4PrgaSig[] = {{")
                print(f"    {format_signature(sig)}")
                print(f"}};")
                print(f"// Stable across {len(results)} modules, {prefix_len} bytes")
            else:
                print("// WARNING: No stable prefix >= 6 bytes across modules")
                print("// Use heuristic MOVZX cluster scanner instead")
        else:
            sig = results[0]['signature']
            print(f"static constexpr uint8_t kRC4PrgaSig[] = {{")
            print(f"    {format_signature(sig)}")
            print(f"}};")
            print(f"// From single module {results[0]['hash'][:8]}")


if __name__ == '__main__':
    main()
