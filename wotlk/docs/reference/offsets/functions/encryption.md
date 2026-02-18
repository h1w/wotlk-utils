# Encryption & Security — WoW 3.3.5a (build 12340)

> Source: offsets.txt, sections "Object Funcs 1/2/3"

## Hooked Functions

### ARC4__Process
- **Address**: `0x00774EA0`
- **Convention**: `__thiscall`
- **Signature**: `void(void* data, int length)`
- **Status**: HOOKED (hooks.cpp:147)
- **Notes**: Session header cipher, NOT Warden payload cipher

---

## All Functions

| Address | Name |
|---------|------|
| `0x00464580` | SecureRandom__GetHash |
| `0x004C1510` | SecureRandom__Seed |
| `0x006CA180` | SHA1__Update2 |
| `0x006CA270` | SHA1__Final2 |
| `0x006CB5F0` | SHA1__Init |
| `0x006CB630` | SHA1__Update |
| `0x006CB6F0` | SHA1__Final |
| `0x00774EA0` | ARC4__Process |
| `0x00775040` | ARC4__Init |
| `0x0077A560` | SHA1Broken__UpdateInternal |
| `0x0077AAA0` | SHA1Broken__Init |
| `0x0077AAE0` | SHA1Broken__Update |
| `0x0077ABA0` | SHA1Broken__Final |
| `0x0086D640` | GenSecureRandom |
