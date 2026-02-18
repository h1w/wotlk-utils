"""
Validate ALL Warden modules: extract exactly 9 check type IDs from each.

Every Warden module has 9 check types corresponding to:
  TIMING(0B), LUA(1B), MPQ(1B), MEM(6B), MODULE(24B),
  DRIVER(25B), PAGE_A(29B), PAGE_B(29B), PROC(31B)

Strategy:
  1. RLE-unpack decompressed module to runtime image
  2. XOR-anchored scan for dispatch chains (primary)
  3. Remap cross-reference (fallback for remap-only modules)
  4. Dispatch chain + remap supplement (hybrid)

Usage:
  python validate_all_modules.py
  python validate_all_modules.py -v
  python validate_all_modules.py -m 7C4ABC97B86494A2D91820785F3A1C87
"""

import struct
import zlib
import argparse
from pathlib import Path
from collections import deque

DUMP_DIR = Path(r"C:\Users\bpqvg\source\repos\wotlk-utils\wotlk\docs\warden_dumps")

# Reference: module 7C4ABC97 (fully confirmed)
KNOWN_7C4ABC97 = {
    0x1F: 'TIMING', 0x22: 'PAGE_A', 0x47: 'PAGE_B',
    0x69: 'PROC', 0x8E: 'MEM', 0x91: 'MPQ',
    0xB3: 'MODULE', 0xD8: 'DRIVER', 0xDB: 'LUA',
}

# Check data sizes (identical across all modules)
SIZE_TO_NAME = {0: 'TIMING', 1: 'MPQ/LUA', 6: 'MEM', 24: 'MODULE',
                25: 'DRIVER', 29: 'PAGE', 31: 'PROC'}

# C++ algorithm constants
MAX_MOVZX_DIST = 18
CHAIN_SCAN_LEN = 400
MAX_BRANCHES = 16
MIN_CHAIN_TYPES = 3
MAX_JUMP_DIST = 10
REMAP_CHECK_DIST = 60


# ============================================================
# Module loading & RLE unpacking
# ============================================================

def load_module(module_hash):
    """Load a warden module. Returns (data, needs_unpack) or (None, False).

    needs_unpack=True  → packed decompressed binary, must call unpack_rle()
    needs_unpack=False → runtime image (from memory), already unpacked
    """
    # Try decompressed first (packed binary → needs RLE unpack)
    p = DUMP_DIR / f"warden_{module_hash}_decompressed.bin"
    if p.exists():
        return p.read_bytes(), True
    # Fallback: decompress from decrypted (also packed → needs unpack)
    p = DUMP_DIR / f"warden_{module_hash}_decrypted.bin"
    if p.exists():
        raw = p.read_bytes()
        if len(raw) < 5:
            return None, False
        # [4-byte LE decompressed size] [zlib stream]
        if raw[4] != 0x78:
            return None, False
        try:
            return zlib.decompress(raw[4:]), True
        except zlib.error:
            return None, False
    # Fallback: runtime image captured from process memory (already unpacked)
    p = DUMP_DIR / f"warden_{module_hash}_inmemory.bin"
    if p.exists():
        return p.read_bytes(), False
    return None, False


def unpack_rle(data):
    """RLE-unpack decompressed module into runtime image."""
    if len(data) < 0x28:
        raise ValueError(f"Too small for header: {len(data)} bytes")

    fields = struct.unpack_from('<10I', data, 0)
    module_size = fields[0]
    section_count = fields[9]

    if module_size == 0 or module_size > 256 * 1024:
        raise ValueError(f"Bad moduleSize: {module_size}")

    # Parse section descriptors
    sections = []
    for i in range(section_count):
        off = 0x28 + i * 12
        if off + 12 > len(data):
            break
        vaddr, vsize, flags = struct.unpack_from('<3I', data, off)
        sections.append((vaddr, vsize, flags))

    if not sections:
        raise ValueError("No sections")

    image = bytearray(module_size)
    image[0:0x28] = data[0:0x28]

    src = 0x28 + section_count * 12
    dst = sections[0][0]
    is_skip = False

    while dst < module_size and src + 2 <= len(data):
        length = struct.unpack_from('<H', data, src)[0]
        src += 2
        if not is_skip:
            if src + length > len(data) or dst + length > module_size:
                break
            image[dst:dst + length] = data[src:src + length]
            src += length
        dst += length
        is_skip = not is_skip

    return bytes(image)


# ============================================================
# Raw byte helpers
# ============================================================

def read_s8(data, off):
    v = data[off]
    return v - 256 if v >= 128 else v


def read_s32(data, off):
    v = struct.unpack_from('<i', data, off)[0]
    return v


def find_cond_jump(data, pos, max_dist=MAX_JUMP_DIST):
    """Find conditional jump within max_dist bytes. Returns (jcc_offset, opcode) or None.
    Handles both short (7x) and near (0F 8x) conditional jumps."""
    end = min(pos + max_dist, len(data) - 1)
    for i in range(pos, end):
        b = data[i]
        # Short Jcc: 70-7F
        if 0x70 <= b <= 0x7F:
            return i, b
        # Near Jcc: 0F 80-8F
        if b == 0x0F and i + 1 < len(data) and 0x80 <= data[i + 1] <= 0x8F:
            return i, data[i + 1] + 0x10  # normalize: 0x80->0x90, etc. — keep as-is actually
            # Actually let's keep the raw second byte and handle both
    return None


def jump_target(data, jcc_off):
    """Compute jump target address. Returns (target, insn_len)."""
    b = data[jcc_off]
    if 0x70 <= b <= 0x7F:
        rel = read_s8(data, jcc_off + 1)
        return jcc_off + 2 + rel, 2
    if b == 0x0F and jcc_off + 5 < len(data) and 0x80 <= data[jcc_off + 1] <= 0x8F:
        rel = read_s32(data, jcc_off + 2)
        return jcc_off + 6 + rel, 6
    return None, 0


def is_je(data, off):
    if off >= len(data):
        return False
    return data[off] == 0x74 or (data[off] == 0x0F and off + 1 < len(data) and data[off + 1] == 0x84)


def je_len(data, off):
    if data[off] == 0x74:
        return 2
    return 6  # 0F 84 xx xx xx xx


# Classify jump condition
JCC_EQ = 1      # je/jz (74, 0F84)
JCC_NE = 2      # jne/jnz (75, 0F85)
JCC_GT = 3      # jg (7F, 0F8F)
JCC_GE = 4      # jge (7D, 0F8D)
JCC_LT = 5      # jl (7C, 0F8C)
JCC_LE = 6      # jle (7E, 0F8E)
JCC_A = 7       # ja (77, 0F87) — unsigned
JCC_OTHER = 99


def classify_jcc(opcode_byte):
    """Classify a Jcc opcode byte (short: 7x, near second byte: 8x)."""
    # Normalize: short 7x and near 8x have same low nibble
    nib = opcode_byte & 0x0F
    if nib == 0x04:
        return JCC_EQ
    if nib == 0x05:
        return JCC_NE
    if nib == 0x0F:
        return JCC_GT
    if nib == 0x0D:
        return JCC_GE
    if nib == 0x0C:
        return JCC_LT
    if nib == 0x0E:
        return JCC_LE
    if nib == 0x07:
        return JCC_A
    return JCC_OTHER


def find_and_classify_jcc(data, pos, max_dist=MAX_JUMP_DIST):
    """Find conditional jump, return (jcc_off, jcc_kind, target, after_jcc) or None."""
    end = min(pos + max_dist, len(data) - 1)
    for i in range(pos, end):
        b = data[i]
        if 0x70 <= b <= 0x7F:
            kind = classify_jcc(b)
            rel = read_s8(data, i + 1)
            return i, kind, i + 2 + rel, i + 2
        if b == 0x0F and i + 5 < len(data) and 0x80 <= data[i + 1] <= 0x8F:
            kind = classify_jcc(data[i + 1])
            rel = read_s32(data, i + 2)
            return i, kind, i + 6 + rel, i + 6
    return None


# ============================================================
# XOR site scanner
# ============================================================

def find_xor_movzx_sites(data):
    """Find all (xor r8,[reg+4]) + (movzx r32,r8) pairs."""
    sites = []
    for i in range(len(data) - 2):
        if data[i] != 0x32:
            continue
        modrm = data[i + 1]
        if not (0x40 <= modrm <= 0x7F):
            continue
        if (modrm & 7) == 4:  # SIB
            continue
        if data[i + 2] != 0x04:
            continue
        # Find movzx r32, r8 within MAX_MOVZX_DIST
        for j in range(i + 3, min(i + 3 + MAX_MOVZX_DIST, len(data) - 2)):
            if data[j] == 0x0F and data[j + 1] == 0xB6 and data[j + 2] >= 0xC0:
                sites.append((i, j, j + 3))
                break
    return sites


# ============================================================
# Runtime base detection (for inmemory images with relocations)
# ============================================================

def detect_runtime_base(data):
    """Detect runtime base address from relocated pointers in an inmemory image.

    In an inmemory dump, all relocatable 4-byte values have had the runtime
    base address added.  We scan the code section for absolute address
    references (MOV eax,[addr] / MOV r32,[addr]) and extract the base by
    rounding down to a 64 KB boundary (VirtualAlloc guarantee on Windows).

    Returns 0 for packed (non-relocated) images.
    """
    if len(data) < 0x28:
        return 0
    module_size = struct.unpack_from('<I', data, 0)[0]

    # Quick heuristic: inmemory images have moduleSize == file size
    if module_size != len(data) or module_size == 0:
        return 0

    # Scan code section for absolute address references.
    # Code typically starts at 0x1000 (first section VA).
    # Require 3+ instructions pointing to the SAME base to avoid false
    # positives on non-relocated (unpack_rle) images where random byte
    # sequences can mimic address patterns.
    candidates = {}  # base -> hit count
    scan_end = min(len(data) - 5, 0x8000)
    for i in range(0x1000, scan_end):
        val = 0
        # MOV eax, [disp32]: A1 XX XX XX XX
        if data[i] == 0xA1:
            val = struct.unpack_from('<I', data, i + 1)[0]
        # MOV r32, [disp32]: 8B [05|0D|15|1D|25|2D|35|3D] XX XX XX XX
        elif data[i] == 0x8B and data[i + 1] in (0x05, 0x0D, 0x15, 0x1D,
                                                   0x25, 0x2D, 0x35, 0x3D):
            val = struct.unpack_from('<I', data, i + 2)[0]
        else:
            continue
        if val > module_size:
            base = val & 0xFFFF0000
            if 0 < val - base < module_size:
                candidates[base] = candidates.get(base, 0) + 1
                if candidates[base] >= 3:
                    return base
    return 0


# ============================================================
# Remap table detection & extraction
# ============================================================

def detect_remap(data, after_movzx, runtime_base=0):
    """Detect remap table pattern. Returns (remap_off, jtable_off, shift, max_type) or None."""
    end = min(after_movzx + REMAP_CHECK_DIST, len(data))
    shift = 0
    max_type = 0xFF

    # Scan for shift and max_type
    i = after_movzx
    while i + 2 < end:
        b = data[i]
        # add r32, sign-ext imm8 (83 C0-C7 XX)
        if b == 0x83 and data[i + 1] >= 0xC0 and data[i + 1] <= 0xC7:
            imm = data[i + 2]
            if imm >= 0x80:
                shift = (0x100 - imm) & 0xFF
            i += 3; continue
        # sub r32, imm8 (83 E8-EF XX)
        if b == 0x83 and data[i + 1] >= 0xE8 and data[i + 1] <= 0xEF:
            imm = data[i + 2]
            if 0 < imm < 0x80:
                shift = imm
            i += 3; continue
        # sub al, imm8 (2C XX)
        if b == 0x2C and i + 1 < end:
            imm = data[i + 1]
            if 0 < imm < 0x80:
                shift = imm
            i += 2; continue
        # cmp r32, imm8 (83 F8-FF XX)
        if b == 0x83 and i + 2 < end and data[i + 1] >= 0xF8 and data[i + 1] <= 0xFF:
            imm = data[i + 2]
            if imm >= 0x80:
                max_type = imm
            i += 3; continue
        # cmp al, imm8 (3C XX)
        if b == 0x3C and i + 1 < end:
            imm = data[i + 1]
            if imm >= 0x80:
                max_type = imm
            i += 2; continue
        # cmp eax, imm32 (3D XX 00 00 00)
        if b == 0x3D and i + 4 < end and data[i + 2] == 0 and data[i + 3] == 0 and data[i + 4] == 0:
            imm = data[i + 1]
            if imm >= 0x80:
                max_type = imm
            i += 5; continue
        i += 1

    # Find movzx byte ptr [reg + disp32] (0F B6 [80-BF, rm!=4])
    for j in range(after_movzx, min(after_movzx + REMAP_CHECK_DIST, len(data) - 6)):
        if data[j] != 0x0F or data[j + 1] != 0xB6:
            continue
        m = data[j + 2]
        if not (0x80 <= m <= 0xBF) or (m & 7) == 4:
            continue
        raw_disp = struct.unpack_from('<I', data, j + 3)[0]
        disp = (raw_disp - runtime_base) & 0xFFFFFFFF
        if disp <= 0x100 or disp >= len(data):
            continue
        # Find jmp [reg*4 + disp32] within 25 bytes
        for k in range(j + 7, min(j + 32, len(data) - 6)):
            if data[k] == 0xFF and data[k + 1] == 0x24:
                sib = data[k + 2]
                if (sib >> 6) == 2 and (sib & 7) == 5:  # scale=4, base=disp32
                    raw_jt = struct.unpack_from('<I', data, k + 3)[0]
                    jt = (raw_jt - runtime_base) & 0xFFFFFFFF
                    return (disp, jt, shift, max_type)
    return None


def _compute_max_handler_idx(remap_off, jtable_off):
    """Compute max valid handler index from jump table size.
    The jump table is an array of 4-byte pointers, located between jtable_off and remap_off."""
    if jtable_off < remap_off:
        gap = remap_off - jtable_off
        if gap >= 4 and gap % 4 == 0:
            return gap // 4 - 1
    return None


def extract_remap_groups(data, remap_off, max_type, shift, jtable_off=0):
    """Extract type IDs from a remap table. Returns set of (shifted) type IDs
    for groups with <= 3 entries (singletons + pairs + triplets)."""
    table_len = min(max_type + 1, len(data) - remap_off)
    if table_len <= 0:
        return set()
    table = data[remap_off:remap_off + table_len]

    # Determine valid handler index range from jump table
    max_handler = _compute_max_handler_idx(remap_off, jtable_off)

    # Find default (most common index among valid entries)
    counts = [0] * 256
    for b in table:
        if max_handler is None or b <= max_handler:
            counts[b] += 1
    default_idx = max(range(256), key=lambda x: counts[x])

    # Group by handler index (only valid indices)
    groups = {}
    for i in range(table_len):
        idx = table[i]
        if idx == default_idx:
            continue
        if max_handler is not None and idx > max_handler:
            continue  # invalid handler index (garbage beyond table)
        groups.setdefault(idx, []).append(i)

    # Collect singletons + pairs + triplets
    result = set()
    for idx, offsets in groups.items():
        if len(offsets) <= 3:
            for raw in offsets:
                result.add((raw + shift) & 0xFF)
    return result


def extract_remap_all(data, remap_off, max_type, shift, jtable_off=0):
    """Extract ALL non-default type IDs from a single remap table."""
    table_len = min(max_type + 1, len(data) - remap_off)
    if table_len <= 0:
        return set()
    table = data[remap_off:remap_off + table_len]

    max_handler = _compute_max_handler_idx(remap_off, jtable_off)

    counts = [0] * 256
    for b in table:
        if max_handler is None or b <= max_handler:
            counts[b] += 1
    default_idx = max(range(256), key=lambda x: counts[x])

    result = set()
    for i in range(table_len):
        idx = table[i]
        if idx != default_idx and (max_handler is None or idx <= max_handler):
            result.add((i + shift) & 0xFF)
    return result


# ============================================================
# Dispatch chain BFS walker
# ============================================================

def walk_chain(data, start, pre_regs=None):
    """BFS dispatch chain walker. Returns set of type IDs."""
    types = set()
    visited = set()
    queue = deque()
    queue.append((start, 0))  # (offset, accumulator)
    branches = 0

    if pre_regs is None:
        pre_regs = {}

    while queue and branches < MAX_BRANCHES:
        origin, acc = queue.popleft()
        if origin in visited or origin < 0 or origin >= len(data):
            continue

        pos = origin
        scan_end = min(origin + CHAIN_SCAN_LEN, len(data))
        regs = dict(pre_regs)
        push_stack = []  # track push imm8 values for pop reg

        while pos < scan_end:
            if pos in visited and pos != origin:
                break
            visited.add(pos)
            b0 = data[pos]

            # === Stop conditions ===
            if b0 in (0xC3, 0xC9, 0xCC):  # ret, leave, int3
                break
            if b0 == 0xE8:  # call
                break
            if 0x50 <= b0 <= 0x57 or b0 == 0x68:  # push reg / push imm32
                break
            if b0 == 0xFF and pos + 1 < scan_end and (data[pos + 1] & 0x38) == 0x10:  # call [reg]
                break
            if b0 == 0x8D and pos + 1 < scan_end:  # lea
                modrm = data[pos + 1]
                rm = modrm & 7
                mod = modrm >> 6
                if mod != 3 and rm in (4, 5):
                    break

            handled = False

            # === CMP eax, imm32 (3D XX 00 00 00) ===
            if not handled and b0 == 0x3D and pos + 4 < scan_end:
                if data[pos + 2] == 0 and data[pos + 3] == 0 and data[pos + 4] == 0:
                    imm = data[pos + 1]
                    r = find_and_classify_jcc(data, pos + 5)
                    if r and imm <= 0xFF:
                        joff, jk, tgt, after = r
                        handled = _handle_cmp(types, queue, imm, jk, tgt, after, acc, data, pos)
                        if handled == 'break':
                            break
                        if handled:
                            pos = handled
                            continue
                    if not handled:
                        pos += 5; continue

            # === CMP r32, imm8 (83 F8-FF XX) ===
            if not handled and b0 == 0x83 and pos + 2 < scan_end:
                modrm = data[pos + 1]
                if 0xF8 <= modrm <= 0xFF:
                    imm = data[pos + 2]
                    r = find_and_classify_jcc(data, pos + 3)
                    if r and imm <= 0xFF:
                        joff, jk, tgt, after = r
                        handled = _handle_cmp(types, queue, imm, jk, tgt, after, acc, data, pos)
                        if handled == 'break':
                            break
                        if handled:
                            pos = handled
                            continue
                    if not handled:
                        pos += 3; continue

            # === CMP al, imm8 (3C XX) ===
            if not handled and b0 == 0x3C and pos + 1 < scan_end:
                imm = data[pos + 1]
                r = find_and_classify_jcc(data, pos + 2)
                if r and imm <= 0xFF:
                    joff, jk, tgt, after = r
                    handled = _handle_cmp(types, queue, imm, jk, tgt, after, acc, data, pos)
                    if handled == 'break':
                        break
                    if handled:
                        pos = handled
                        continue
                if not handled:
                    pos += 2; continue

            # === CMP r32, r32 (3B C0-FF) ===
            if not handled and b0 == 0x3B and pos + 1 < scan_end and data[pos + 1] >= 0xC0:
                modrm = data[pos + 1]
                reg2 = modrm & 7
                if reg2 in regs and regs[reg2] <= 0xFF:
                    imm = regs[reg2]
                    r = find_and_classify_jcc(data, pos + 2)
                    if r:
                        joff, jk, tgt, after = r
                        handled = _handle_cmp(types, queue, imm, jk, tgt, after, acc, data, pos)
                        if handled == 'break':
                            break
                        if handled:
                            pos = handled
                            continue
                if not handled:
                    pos += 2; continue

            # === SUB r32, imm8 (83 E8-EF XX) ===
            if not handled and b0 == 0x83 and pos + 2 < scan_end:
                modrm = data[pos + 1]
                if 0xE8 <= modrm <= 0xEF:
                    delta = data[pos + 2]
                    r = find_and_classify_jcc(data, pos + 3)
                    if r:
                        joff, jk, tgt, after = r
                        if jk in (JCC_EQ, JCC_NE):
                            acc += delta
                            types.add(acc & 0xFF)
                            if jk == JCC_NE:
                                queue.append((tgt, acc))
                                branches += 1
                                break
                            pos = after; handled = True; continue
                    if not handled:
                        pos += 3; continue

            # === SUB al, imm8 (2C XX) ===
            if not handled and b0 == 0x2C and pos + 1 < scan_end:
                delta = data[pos + 1]
                r = find_and_classify_jcc(data, pos + 2)
                if r:
                    joff, jk, tgt, after = r
                    if jk in (JCC_EQ, JCC_NE):
                        acc += delta
                        types.add(acc & 0xFF)
                        if jk == JCC_NE:
                            queue.append((tgt, acc))
                            branches += 1
                            break
                        pos = after; handled = True; continue
                if not handled:
                    pos += 2; continue

            # === SUB eax, imm32 byte-range (2D XX 00 00 00) ===
            if not handled and b0 == 0x2D and pos + 4 < scan_end:
                if data[pos + 2] == 0 and data[pos + 3] == 0 and data[pos + 4] == 0:
                    delta = data[pos + 1]
                    r = find_and_classify_jcc(data, pos + 5)
                    if r:
                        joff, jk, tgt, after = r
                        if jk in (JCC_EQ, JCC_NE):
                            acc += delta
                            types.add(acc & 0xFF)
                            if jk == JCC_NE:
                                queue.append((tgt, acc))
                                branches += 1
                                break
                            pos = after; handled = True; continue
                if not handled:
                    pos += 5; continue

            # === DEC r32 (48-4F) ===
            if not handled and 0x48 <= b0 <= 0x4F:
                r = find_and_classify_jcc(data, pos + 1)
                if r:
                    joff, jk, tgt, after = r
                    if jk in (JCC_EQ, JCC_NE):
                        acc += 1
                        types.add(acc & 0xFF)
                        if jk == JCC_NE:
                            queue.append((tgt, acc))
                            branches += 1
                            break
                        pos = after; handled = True; continue
                if not handled:
                    pos += 1; continue

            # === TEST r32, r32 (85 C0-FF with reg1==reg2) ===
            if not handled and b0 == 0x85 and pos + 1 < scan_end and data[pos + 1] >= 0xC0:
                modrm = data[pos + 1]
                reg1 = (modrm >> 3) & 7
                reg2 = modrm & 7
                if reg1 == reg2:
                    r = find_and_classify_jcc(data, pos + 2)
                    if r:
                        joff, jk, tgt, after = r
                        if jk == JCC_EQ:
                            types.add(acc & 0xFF)
                            pos = after; handled = True; continue

            # === PUSH imm8 (6A XX) — track for pop+cmp pattern ===
            if not handled and b0 == 0x6A and pos + 1 < scan_end:
                imm = data[pos + 1]
                push_stack.append(imm)
                # Skip following je/jne (handles previous cmp equality, already recorded)
                r = find_and_classify_jcc(data, pos + 2)
                if r:
                    joff, jk, tgt, after = r
                    if jk in (JCC_EQ, JCC_NE):
                        pos = after; handled = True; continue
                pos += 2; handled = True; continue

            # === POP r32 (58-5F) — assign last pushed imm8 to register ===
            if not handled and 0x58 <= b0 <= 0x5F:
                reg = b0 - 0x58
                if push_stack:
                    regs[reg] = push_stack.pop()
                pos += 1; handled = True; continue

            # === JMP rel8 (EB XX) ===
            if not handled and b0 == 0xEB and pos + 1 < scan_end:
                rel = read_s8(data, pos + 1)
                tgt = pos + 2 + rel
                if 0 <= tgt < len(data):
                    queue.append((tgt, acc))
                    branches += 1
                break

            # === JMP rel32 (E9 XX XX XX XX) ===
            if not handled and b0 == 0xE9 and pos + 4 < scan_end:
                rel = read_s32(data, pos + 1)
                tgt = pos + 5 + rel
                if 0 <= tgt < len(data):
                    queue.append((tgt, acc))
                    branches += 1
                break

            # === MOV r32, imm32 (B8-BF) — register tracking ===
            if not handled and 0xB8 <= b0 <= 0xBF and pos + 4 < scan_end:
                reg = b0 - 0xB8
                imm = struct.unpack_from('<I', data, pos + 1)[0]
                if imm <= 0xFF:
                    regs[reg] = imm
                pos += 5; continue

            # Default: advance 1
            if not handled:
                pos += 1

    return types


def _handle_cmp(types, queue, imm, jk, tgt, after, acc, data, pos):
    """Handle CMP + Jcc pattern. Returns next pos, 'break', or False."""
    types.add(imm)

    if jk == JCC_EQ:
        return after

    if jk == JCC_NE:
        queue.append((tgt, acc))
        return 'break'

    if jk in (JCC_GT, JCC_GE):  # jg, jge — greater branch
        queue.append((tgt, 0))
        # Skip je if immediately after
        if is_je(data, after):
            return after + je_len(data, after)
        return after

    if jk == JCC_LE:  # jle — less-or-equal branch
        queue.append((tgt, 0))
        return after

    if jk == JCC_LT:  # jl — strict less
        queue.append((tgt, 0))
        if is_je(data, after):
            return after + je_len(data, after)
        return after

    if jk == JCC_A:  # ja (unsigned above) — treat like jg
        queue.append((tgt, 0))
        if is_je(data, after):
            return after + je_len(data, after)
        return after

    return False


# ============================================================
# Main analysis pipeline
# ============================================================

def analyze_module(data, verbose=False):
    """Analyze unpacked module image. Returns (types: set, strategy: str, details: str)."""
    runtime_base = detect_runtime_base(data)
    if verbose and runtime_base:
        print(f"  Runtime base: 0x{runtime_base:08X} (inmemory, relocations adjusted)")

    sites = find_xor_movzx_sites(data)
    if verbose:
        print(f"  XOR+MOVZX sites: {len(sites)}")

    chain_types_all = set()
    chain_sets = []  # per-chain type sets (for intersection)
    chain_count = 0
    remaps = []  # list of (remap_off, jtable_off, shift, max_type)

    for xor_off, movzx_off, after_movzx in sites:
        # Try remap detection first
        r = detect_remap(data, after_movzx, runtime_base)
        if r:
            remaps.append(r)
            if verbose:
                roff, jtoff, sh, mx = r
                print(f"    XOR @0x{xor_off:04X}: remap (remap=0x{roff:04X} shift=0x{sh:02X} max=0x{mx:02X})")
            continue

        # Pre-scan registers between XOR and movzx
        pre_regs = {}
        for p in range(xor_off, movzx_off):
            if p + 4 < len(data) and 0xB8 <= data[p] <= 0xBF:
                reg = data[p] - 0xB8
                imm = struct.unpack_from('<I', data, p + 1)[0]
                if imm <= 0xFF:
                    pre_regs[reg] = imm

        # Walk dispatch chain
        types = walk_chain(data, after_movzx, pre_regs)
        if types:
            chain_count += 1
            chain_types_all |= types
            chain_sets.append(set(types))
            if verbose:
                print(f"    XOR @0x{xor_off:04X}: dispatch chain, {len(types)} types: "
                      f"{' '.join(f'0x{t:02X}' for t in sorted(types))}")

    # Strategy 1: Dispatch chain found
    if len(chain_types_all) >= MIN_CHAIN_TYPES:
        result = set(chain_types_all)
        strategy = f"dispatch({chain_count})"

        # When multiple chains disagree (union > 10), intersection removes phantom BST pivots
        if chain_count >= 2 and len(result) > 10 and len(chain_sets) >= 2:
            chain_intersection = chain_sets[0]
            for s in chain_sets[1:]:
                chain_intersection = chain_intersection & s
            if 9 <= len(chain_intersection) <= 10:
                result = chain_intersection
                strategy = f"dispatch_intersection({chain_count})"
            else:
                # Intersection too small — use the single best chain (most complete)
                best = max(chain_sets, key=len)
                if 9 <= len(best) <= 10:
                    result = best
                    strategy = f"dispatch_best({chain_count})"

        # Supplement with remap if incomplete
        if len(result) < 9 and remaps:
            # Try cross-ref first (>=2 remap tables)
            if len(remaps) >= 2:
                remap_types = _remap_crossref(data, remaps, verbose)
                if len(remap_types) > len(result):
                    result = remap_types
                    strategy = f"dispatch({chain_count})+remap_xref({len(remaps)})"
            # Single remap supplement
            if len(result) < 9 and len(remaps) >= 1:
                for roff, jtoff, sh, mx in remaps:
                    fixed_mx = _fix_max_type_capstone(data, roff, jtoff, sh, mx, runtime_base)
                    remap_all = extract_remap_all(data, roff, fixed_mx, sh, jtable_off=jtoff)
                    if 9 <= len(remap_all) <= 12:
                        result = remap_all
                        strategy = f"dispatch({chain_count})+single_remap_fixed"
                        break

        details = f"{chain_count}d+{len(remaps)}r"
        return result, strategy, details

    # Strategy 2: Remap cross-reference (>=2 remap tables)
    if len(remaps) >= 2:
        remap_types = _remap_crossref(data, remaps, verbose)
        if 9 <= len(remap_types) <= 10:
            return remap_types, f"remap_xref({len(remaps)})", f"0d+{len(remaps)}r"

    # Strategy 3: Best single remap (pick table with entry count closest to 9)
    if remaps:
        best_types, best_strategy = None, None
        for roff, jtoff, sh, mx in remaps:
            # Fix max_type using capstone if still 0xFF
            fixed_mx = _fix_max_type_capstone(data, roff, jtoff, sh, mx, runtime_base)
            types = extract_remap_all(data, roff, fixed_mx, sh, jtable_off=jtoff)
            if verbose and fixed_mx != mx:
                print(f"    Remap @0x{roff:04X}: max_type fixed 0x{mx:02X}->0x{fixed_mx:02X}, "
                      f"entries={len(types)}")
            if 9 <= len(types) <= 10:
                if best_types is None or abs(len(types) - 9) < abs(len(best_types) - 9):
                    best_types = types
                    best_strategy = f"best_remap(@0x{roff:04X})"
        if best_types:
            return best_types, best_strategy, f"0d+{len(remaps)}r"

    # Strategy 4: Remap cross-ref with relaxed threshold
    if len(remaps) >= 2:
        remap_types = _remap_crossref(data, remaps, verbose)
        if len(remap_types) >= MIN_CHAIN_TYPES:
            return remap_types, f"remap_xref({len(remaps)})", f"0d+{len(remaps)}r"

    # Strategy 5: Retry chain walk on remap-classified XOR sites
    if remaps:
        for roff, jtoff, sh, mx in remaps:
            for xor_off, movzx_off, after_movzx in sites:
                r = detect_remap(data, after_movzx)
                if r and r[0] == roff:
                    types = walk_chain(data, after_movzx)
                    if len(types) >= MIN_CHAIN_TYPES:
                        return types, "remap_site_retry", f"0d+{len(remaps)}r"

    return set(), "NONE", f"{chain_count}d+{len(remaps)}r"


def _fix_max_type_capstone(data, remap_off, jtable_off, shift, max_type, runtime_base=0):
    """Fix max_type using capstone disassembly when byte-level detection missed it."""
    if max_type != 0xFF:
        return max_type  # already detected
    try:
        from capstone import Cs, CS_ARCH_X86, CS_MODE_32
    except ImportError:
        return max_type

    md = Cs(CS_ARCH_X86, CS_MODE_32)

    # Step 1: Find the code that references the remap table.
    # Look for: movzx reg, byte ptr [reg + remap_off] encoded as 0F B6 [80-BF] [LE32 remap_off]
    # In inmemory images, the disp32 in the instruction is (runtime_base + remap_off).
    remap_le = struct.pack('<I', remap_off + runtime_base)
    code_addr = None
    for i in range(len(data) - 7):
        if (data[i] == 0x0F and data[i + 1] == 0xB6
                and 0x80 <= data[i + 2] <= 0xBF and (data[i + 2] & 7) != 4
                and data[i + 3:i + 7] == remap_le):
            code_addr = i
            break

    if code_addr is None:
        return max_type

    # Step 2: Disassemble backwards from code_addr to find CMP + JA/JAE.
    # Search up to 60 bytes before the remap movzx instruction.
    search_start = max(0, code_addr - 60)
    last_cmp_imm = None

    best_cmp = None
    for insn in md.disasm(data[search_start:code_addr + 7], search_start):
        if insn.address >= code_addr:
            break
        if insn.mnemonic == 'cmp':
            parts = insn.op_str.split(',')
            if len(parts) == 2:
                imm_str = parts[1].strip()
                try:
                    imm = int(imm_str, 0)
                    if 0 < imm <= 0xFF:
                        last_cmp_imm = imm
                except ValueError:
                    pass
        elif insn.mnemonic in ('ja', 'jae') and last_cmp_imm is not None:
            best_cmp = last_cmp_imm

    return best_cmp if best_cmp is not None else max_type


def _remap_crossref(data, remaps, verbose):
    """Cross-reference multiple remap tables via singleton+pair+triplet intersection."""
    sp_sets = []
    for roff, jtoff, sh, mx in remaps:
        sp = extract_remap_groups(data, roff, mx, sh, jtable_off=jtoff)
        sp_sets.append(sp)
        if verbose:
            max_h = _compute_max_handler_idx(roff, jtoff)
            extra = f" max_handler={max_h}" if max_h is not None else ""
            print(f"    Remap @0x{roff:04X}: {len(sp)} singleton/pair/triplet candidates{extra}")

    if len(sp_sets) < 2:
        return set()

    # Intersection
    result = set(sp_sets[0])
    for s in sp_sets[1:]:
        result &= s

    if verbose and result:
        print(f"    Cross-ref intersection: {len(result)} types: "
              f"{' '.join(f'0x{t:02X}' for t in sorted(result))}")
    return result


# ============================================================
# Main
# ============================================================

def main():
    parser = argparse.ArgumentParser(description='Validate all Warden modules')
    parser.add_argument('-v', '--verbose', action='store_true')
    parser.add_argument('-m', '--module', help='Single module hash (full 32-char)')
    args = parser.parse_args()

    # Discover modules
    hashes = set()
    for f in DUMP_DIR.iterdir():
        name = f.name
        if name.startswith('warden_') and (name.endswith('_decompressed.bin') or name.endswith('_decrypted.bin') or name.endswith('_inmemory.bin')):
            h = name.split('_')[1]
            if len(h) == 32:
                hashes.add(h)
    hashes = sorted(hashes)

    if args.module:
        matches = [h for h in hashes if args.module.upper() in h.upper()]
        if not matches:
            print(f"Module not found: {args.module}")
            return
        hashes = matches

    print(f"Analyzing {len(hashes)} module(s)\n")

    results = []
    for h in hashes:
        short = h[:8]
        data, needs_unpack = load_module(h)
        if data is None:
            print(f"  {short}: LOAD FAILED")
            results.append((h, set(), "load_fail", ""))
            continue

        if needs_unpack:
            try:
                unpacked = unpack_rle(data)
            except Exception as e:
                print(f"  {short}: UNPACK FAILED ({e})")
                results.append((h, set(), "unpack_fail", ""))
                continue
            source = "decompressed"
        else:
            unpacked = data
            source = "inmemory"

        if args.verbose:
            print(f"Module {short} ({h}) [{source}]")
            if needs_unpack:
                print(f"  Decompressed: {len(data)} bytes, Unpacked: {len(unpacked)} bytes")
            else:
                print(f"  Runtime image: {len(data)} bytes")

        types, strategy, details = analyze_module(unpacked, verbose=args.verbose)

        count = len(types)
        ok = 9 <= count <= 10  # 10 is OK (BST pivot phantom)
        status = "OK" if ok else f"FAIL({count})"

        if args.verbose:
            print(f"  Result: {count} types [{strategy}]")
            if types:
                print(f"  Types: {' '.join(f'0x{t:02X}' for t in sorted(types))}")

            # Validate against known module
            if short == '7C4ABC97':
                known = set(KNOWN_7C4ABC97.keys())
                if types == known:
                    print(f"  REFERENCE VALIDATED: all 9 types match 7C4ABC97")
                else:
                    missing = known - types
                    extra = types - known
                    if missing:
                        print(f"  MISSING: {' '.join(f'0x{t:02X}={KNOWN_7C4ABC97[t]}' for t in sorted(missing))}")
                    if extra:
                        print(f"  EXTRA:   {' '.join(f'0x{t:02X}' for t in sorted(extra))}")
            print()
        else:
            print(f"  {short}: {status:10s} [{strategy:30s}] {details}")

        results.append((h, types, strategy, details))

    # Summary
    total = len(results)
    ok_count = sum(1 for _, types, _, _ in results if 9 <= len(types) <= 10)
    print(f"\n{'=' * 70}")
    print(f"RESULTS: {ok_count}/{total} modules with 9-10 types ({100 * ok_count // total if total else 0}%)")

    failures = [(h, types, strat) for h, types, strat, _ in results if not (9 <= len(types) <= 10)]
    if failures:
        print(f"\nFailed modules:")
        for h, types, strat in failures:
            print(f"  {h[:8]}: {len(types)} types ({strat}) — {' '.join(f'0x{t:02X}' for t in sorted(types))}")


if __name__ == '__main__':
    main()
