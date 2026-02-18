# Warden Anti-Cheat — WoW 3.3.5a (build 12340)

> Source: offsets.txt, sections "Object Funcs 1/2/3"

## Hooked Functions

### SMSG_WARDEN_DATA Handler
- **Address**: `0x007DA850`
- **Convention**: `non-standard (naked hook required)`
- **Signature**: `custom stack layout [ret][0][opcode=0x2E6][ECX_dup][CDataStore*]`
- **Status**: HOOKED (hooks.cpp:101)
- **Notes**: SMSG_WARDEN_DATA handler, return-address hijack for post-decrypt access

---

## All Functions

| Address | Name |
|---------|------|
| `0x00676AC0` | DBCache_Warden |
| `0x006791B0` | DBCache_WardenCachedModule__InternalDelete |
| `0x0067FA80` | DbWoWCache_GetInfoBlockById |
| `0x0067FE00` | DbWoWCache_Load |
| `0x007C07C0` | Alloc_WAREA |
| `0x007DA930` | DbWoWCache_Shutdown_WARDEN_UNLOAD |
