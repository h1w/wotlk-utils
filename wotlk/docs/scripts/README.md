# Warden Analysis Scripts

Python scripts for offline analysis of Warden module dumps (`warden_dumps/`).

## Directory Structure

### `module_format/` — Module Binary Format

| Script | Description |
|--------|-------------|
| `unpack_rle.py` | RLE unpacker: converts `_decompressed.bin` into runtime memory image. Supports `--all` for batch processing, `--info` for header/section details, `-o` for writing unpacked image. |

### `dispatch/` — Type ID Extraction from Dispatch Patterns

| Script | Description |
|--------|-------------|
| `find_request_parsers.py` | XOR-anchored scan: finds `xor r8,[reg+4]` + `movzx` patterns, extracts type IDs from BST dispatch chains. Breakthrough script. |
| `comprehensive_analysis.py` | Dispatch chain analysis across all modules. Classifies XOR sites as remap vs dispatch, unions chain types. |
| `comprehensive_analysis_v2.py` | V2 with BFS queue, register tracking, extended search windows. Matches C++ implementation. |
| `extract_types_v2.py` | Type extractor with recursive branch following and cmp+jne handling. |
| `extract_all_types.py` | Remap table analysis across all modules: groups entries by handler index. |
| `analyze_dispatcher.py` | Capstone-based dispatcher pattern analysis (disassembly). |
| `analyze_remap.py` | Remap + jump table pattern analysis (movzx + cmp + remap + jmp). |
| `group_sizes.py` | Remap table group size analysis: singletons/small groups = real check types. |
| `remap_debug.py` | Debug remap tables for specific modules, grouped by index value. |
| `remap_crossref_test.py` | Test pair-based remap cross-referencing with tuple deduplication. |

### `rc4/` — RC4 Encryption Analysis

| Script | Description |
|--------|-------------|
| `find_rc4_function.py` | Finds RC4 PRGA functions in module binaries via MOVZX displacement scan (0x100/0x101 offsets). Extracts function prologues. |

### `analysis/` — General Module Analysis

| Script | Description |
|--------|-------------|
| `deep_analysis.py` | Multi-strategy analysis of all modules: XOR patterns, classification, type extraction. |
| `scan_all_modules.py` | Bulk scan: decompresses encrypted files, searches for remap tables, cmp/je chains. |
| `analyze_9a95.py` | Deep dive into module 9A95D199 (flexible pattern matching). |
| `extract_types.py` | Early remap table extraction approach using capstone. |

### `verification/` — Testing & Module-Specific Debugging

| Script | Description |
|--------|-------------|
| `verify_pattern.py` | Validates byte patterns for C++ scanner (movzx + jmp detection). |
| `verify_1a61.py` | Verifies module 1A615549 dispatch chain (7 vs 9 types). |
| `investigate_missing.py` | Investigates modules with no XOR patterns (85F902, 9A95D199). |
| `investigate_two.py` | Investigates remap-only modules (0AC0C559, E348326F). |

## Usage

Most scripts expect `_decompressed.bin` files in `warden_dumps/` relative to the game directory or as CLI arguments.

```bash
# Unpack all modules
python module_format/unpack_rle.py --all "Z:/Games/wow 3.3.5a client/warden_dumps"

# Analyze dispatch chains across all modules
python dispatch/find_request_parsers.py

# Find RC4 functions
python rc4/find_rc4_function.py
```

## Dependencies

- Python 3.8+
- `capstone` (for disassembly-based scripts in `dispatch/` and `analysis/`)
