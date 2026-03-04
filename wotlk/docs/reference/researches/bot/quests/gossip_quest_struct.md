# Gossip Quest Data Structure — WoW 3.3.5a Build 12340

> **Status**: Complete
> **Date**: 2026-03-04
> **Method**: Static disassembly (capstone, Python) of CGQuestInfo functions
> **Implements**: TASK_010

---

## Summary

The WoW client stores gossip quest data (both available and active) in a **single flat array** of 32 slots at `0x00BFC968`. Each slot is **532 bytes (0x214)**. The array is populated by the `SMSG_GOSSIP_MESSAGE` packet handler and cleared on `SMSG_GOSSIP_COMPLETE` / next interaction.

Available vs active is determined by the `questIcon` field inside each entry — **no separate arrays**.

---

## GossipQuestEntry Structure

```
Base address: 0x00BFC968
Stride:       0x214 (532 bytes)
Max entries:  32

struct GossipQuestEntry {         // total: 0x214 bytes
    uint32_t questId;       // +0x000  Quest ID (0 = unused slot, marks end)
    int32_t  questLevel;    // +0x004  Level (-1 = profession/special quest)
    uint32_t questFlags;    // +0x008  Quest flags:
                            //           0x1000 = daily
                            //           0x0040 = repeatable
    uint32_t autoComplete;  // +0x00C  Auto-accept byte from packet (0 or 1)
    uint32_t questIcon;     // +0x010  Icon type:
                            //           3 = active, incomplete (gray ?)
                            //           4 = active, completable (yellow ?)
                            //           other = available (yellow !)
    char     title[0x200];  // +0x014  Null-terminated quest name (512 bytes)
};
```

---

## Available vs Active Discrimination

```cpp
bool isActive    = (questIcon == 3 || questIcon == 4);
bool isAvailable = !isActive;
```

This logic is confirmed from `GetNumAvailGossipQuests` (`0x0058A5D0`) and `GetNumActiveGossipQuests` (`0x0058A6C0`) — both walk the same array and count by icon value.

---

## Global State Addresses

| Address       | Type       | Description                                |
|---------------|------------|--------------------------------------------|
| `0x00BFC968`  | `Entry[32]`| Gossip quest entry array (base)            |
| `0x00C016F0`  | `uint64_t` | Gossip NPC object GUID (lo dword at +0)    |
| `0x00C016F8`  | `uint32_t` | Gossip menu text ID (from packet)          |
| `0x00C016FC`  | `uint32_t` | Gossip option count (text buttons)         |

---

## Relevant Functions

| Address       | Name                                  | Signature                                   |
|---------------|---------------------------------------|---------------------------------------------|
| `0x0058A550`  | `SetGossipObjectGUID`                 | `void(uint64_t* guid)` — stores NPC GUID, clears data |
| `0x0058A5D0`  | `CGQuestInfo_C::GetNumAvailGossipQuests`  | `int()` — count of available quests     |
| `0x0058A660`  | `CGQuestInfo_C::GetAvailableQuestInfoFromIndex` | `GossipQuestEntry*(int idx0)` — pointer to entry |
| `0x0058A6C0`  | `CGQuestInfo_C::GetNumActiveGossipQuests`    | `int()` — count of active quests         |
| `0x0058A750`  | `CGQuestInfo_C::GetActiveQuestFromIndex`     | `GossipQuestEntry*(int idx0)` — pointer to entry |
| `0x0058A7B0`  | `ClearGossipQuests`                   | `void()` — zeros all entry questId fields  |
| `0x0058B1B0`  | `Packet_SMSG_GOSSIP_MESSAGE`          | Packet handler — populates the entry array  |
| `0x0058B3A0`  | `lua_GetGossipAvailableQuests`        | C handler for Lua API                       |
| `0x0058B490`  | `lua_GetGossipActiveQuests`           | C handler for Lua API                       |

---

## Disassembly Evidence

### Array iteration pattern (GetNumAvailGossipQuests @ 0x0058A5D0)

```asm
xor eax, eax               ; count = 0
xor ecx, ecx               ; byte offset = 0
loop:
cmp [ecx + 0xBFC968], 0    ; questId == 0 → end of array
je  end
mov edx, [ecx + 0xBFC978]  ; read questIcon (0xBFC978 - 0xBFC968 = +0x10)
cmp edx, 3
je  skip                   ; skip active (icon 3)
cmp edx, 4
je  skip                   ; skip active (icon 4)
add eax, 1                 ; count++ (available quest)
skip:
; ... (loop unrolled 4 entries per iteration)
add ecx, 0x850             ; advance 4 * 0x214
cmp ecx, 0x4280            ; 32 * 0x214
jb  loop
```

### Return-pointer computation (GetAvailableQuestInfoFromIndex found path @ 0x58A6A7)

```asm
imul eax, eax, 0x214       ; eax = rawSlot * stride
add  eax, 0xBFC968         ; eax = &entries[rawSlot]
ret
```

Confirms: the returned pointer is `EntryArrayBase + rawSlot * EntryStride`.

### Packet writer (SMSG_GOSSIP_MESSAGE quest section @ 0x0058B316)

```asm
mov edi, 0xBFC978          ; edi = &entries[0].questIcon
; per-quest loop:
lea eax, [edi - 0x10]      ; &questId   (edi - 0x10)
call ReadUint32             ; → questId
push edi                   ; &questIcon (edi + 0x00)
call ReadUint32             ; → questIcon
lea ecx, [edi - 0x0C]      ; &questLevel
call ReadUint32
lea edx, [edi - 0x08]      ; &questFlags
call ReadUint32
call ReadUint8              ; → autoComplete (temp)
mov [edi - 0x04], ecx      ; store autoComplete at +0x0C
lea edx, [edi + 0x04]      ; &title[0]  (edi + 0x04 = base + 0x14)
call ReadString(512)
add edi, 0x214             ; next entry
```

This confirms all field offsets definitively.

---

## C++ Implementation

- **Header**: `wotlk/src/game/quest.h`
- **Source**: `wotlk/src/game/quest.cpp`
- **Offsets**: `wotlk/src/offsets/gossip.h`

### Key function

```cpp
// 1-based gossipIndex, matching Lua GetGossipAvailableQuests() convention.
// isAvailable: true = yellow !, false = gray/yellow ?
// Returns 0 on failure.
int game::quest::GetGossipQuestId(int gossipIndex, bool isAvailable);
```

### Usage example

```cpp
// After GOSSIP_SHOW fires:
for (int i = 1; i <= game::quest::GetGossipQuestCount(true); ++i) {
    int id = game::quest::GetGossipQuestId(i, true);
    LOG(INFO) << "Available quest [" << i << "]: id=" << id;
}
```

---

## Notes

- The array is contiguous and **zero-terminated**: the first entry with `questId == 0` marks the end of valid data. A full scan to `MaxEntries = 32` is safe.
- `questLevel == -1` is valid (profession quests, some special quests) — do not treat as error.
- `autoComplete` is stored as a full `uint32_t` (zero-extended from the 1-byte packet field).
- The `QUEST_FLAGS_REPEATABLE` flag (`0x0040`) is distinct from `isRepeatable` in the Lua API — both come from `questFlags`.
