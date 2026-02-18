# CDataStore Layout — WoW 3.3.5a (build 12340)

> Internal packet buffer structure used for network I/O

## Memory Layout

| Offset | Type | Field | Notes |
|--------|------|-------|-------|
| +0x00 | `void*` | vtable | Virtual function table pointer |
| +0x04 | `uint8_t*` | buffer | Pointer to raw packet data |
| +0x08 | `uint32_t` | unk_08 | |
| +0x0C | `uint32_t` | unk_0C | |
| +0x10 | `uint32_t` | size | Total buffer size in bytes |
| +0x14 | `uint32_t` | readPos | Current read cursor position |

## Packet Format

### SMSG (Server → Client)

At the SendPacket hook level:
```
[4 bytes] opcode (little-endian)
[N bytes] payload
```

- Opcode `0x2E6` = SMSG_WARDEN_DATA
- Payload is **RC4-encrypted** (Warden payload cipher, inside module blob)

### CMSG (Client → Server)

At the SendPacket hook level:
```
[4 bytes] opcode (little-endian)
[N bytes] payload
```

- Opcode `0x2E7` = CMSG_WARDEN_DATA
- Payload is **RC4-encrypted** at this point

### Session Header Encryption (ARC4)

The session cipher (`ARC4::Process` @ `0x00774EA0`) encrypts/decrypts headers:
- CMSG header: `[2B size][4B opcode]` = 6 bytes
- SMSG header: 4 bytes

## Usage in Our Code

- `hooks.cpp`: `WardenPreHandler` reads CDataStore* from stack (stk4), saves buffer/readPos
- `hooks.cpp`: `WardenPostHandler` reads decrypted data via saved CDataStore*
- `hooks.cpp`: `SendPacketHandler` reads CDataStore* from stack (stk1 after pushad/pushfd)
