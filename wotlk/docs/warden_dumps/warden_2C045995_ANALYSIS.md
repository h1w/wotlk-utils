# Warden Module 2C045995 Analysis

## Module Info
- **Full hash**: 2C0459958D7BF292DD317F66F8611446
- **Decompressed size**: 27558 bytes (0x6BA6)
- **XOR sites found**: 2 (both are request parsers, NOT response builders)

## Dispatch Chain Structure

Both XOR sites (0x10D6 and 0x4AC2) implement the SAME Binary Search Tree:

### Root Node
```
mov ecx, 0x87
cmp eax, ecx
jg  upper_branch    ; if type > 0x87
je  handler_0x87    ; if type == 0x87
; fall through to lower_branch
```

### Lower Branch (type <= 0x87)
```
cmp eax, 0x0E    ; type == 0x0E?
je  handler_0x0E

cmp eax, 0x2D    ; type == 0x2D?
je  handler_0x2D

cmp eax, 0x49    ; type == 0x49?
je  handler_0x49

cmp eax, 0x68    ; type == 0x68?
jne default_handler  ; anything else -> error
jmp handler_0x68
```

### Upper Branch (type > 0x87)
```
sub eax, 0x95    ; Check type == 0x95
je  handler_0x95

sub eax, 0x2D    ; Check type == 0xC2 (0x95 + 0x2D)
je  handler_0xC2

sub eax, 0x1F    ; Check type == 0xE1 (0xC2 + 0x1F)
je  handler_0xE1

sub eax, 0x0E    ; Check type == 0xEF (0xE1 + 0x0E)
jne default_handler  ; anything else -> error
; implicit: je handler_0xEF
```

## Extracted Check Type IDs

**Total: 9 types**

| Type ID | Category | Notes |
|---------|----------|-------|
| 0x0E | ? | |
| 0x2D | ? | |
| 0x49 | ? | |
| 0x68 | ? | |
| 0x87 | ? | Root comparison value |
| 0x95 | ? | |
| 0xC2 | ? | |
| 0xE1 | ? | |
| 0xEF | ? | |

## SUB Chain Verification
The upper branch uses cumulative subtraction:
- 0x95 (direct SUB)
- 0x95 + 0x2D = **0xC2** ✓
- 0xC2 + 0x1F = **0xE1** ✓
- 0xE1 + 0x0E = **0xEF** ✓

## Missing Type 0xA0

**Conclusion: Type 0xA0 is NOT used by this module.**

Analysis:
- 0xA0 is between 0x95 and 0xC2 in the type space
- If it existed, there would be: `sub eax, 0x95; sub eax, 0x0B; je handler`
- No `sub eax, 0x0B` instruction exists in the module
- 0xA0 falls into the default handler (error/reject path)

**Possible explanations:**
1. Type 0xA0 is not used by this module (module uses only 9 types)
2. Type 0xA0 is from a different module's type ID space
3. The "missing 10th type" claim is based on incorrect expectations

## XOR Site Details

### Site #1: Offset 0x10D6
```asm
0x10d6: xor al, byte ptr [esi + 4]     ; XOR with xorByte
0x10d9: mov ecx, 0x87                  ; Load pivot
0x10de: mov byte ptr [ebp - 0x221], al ; Save decrypted type
0x10e4: movzx eax, al                  ; Zero-extend to 32-bit
0x10e7: cmp eax, ecx                   ; Start BST
0x10e9: jle 0x11b1                     ; Lower branch
```

### Site #2: Offset 0x4AC2
```asm
0x4ac2: xor bl, byte ptr [esi + 4]     ; XOR with xorByte
0x4ac5: mov ecx, 0x87                  ; Load pivot
0x4aca: movzx eax, bl                  ; Zero-extend to 32-bit
0x4acd: cmp eax, ecx                   ; Start BST
0x4acf: mov byte ptr [ebp - 0x124], bl ; Save decrypted type
0x4ad5: jg 0x4bea                      ; Upper branch
0x4adb: je 0x4ba3                      ; Handler for 0x87
```

**Both functions implement identical logic with different registers (AL vs BL).**

## Remap Table

**Conclusion: No remap table found.**

- Scanner found no `movzx reg, byte [table + index]; jmp [jtable]` pattern
- Both XOR sites lead directly to BST dispatch chains
- This module uses direct type ID comparison only (no remapping)

## Category Mapping (TODO)

Need to cross-reference with other modules to determine which IDs map to:
- TIMING
- MPQ
- LUA
- MEM
- MODULE
- DRIVER
- PAGE_A / PAGE_B
- PROC

## Notes

- This module has TWO separate request parser functions (unusual)
- Both parsers use identical BST structure
- No response builder found (no remap table for CMSG encoding)
- Default handler at 0x4D5F is an error path (rejects invalid types)
- Module uses only 9 check types (not 10)
