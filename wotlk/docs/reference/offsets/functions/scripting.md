# Scripting (FrameScript) — WoW 3.3.5a (build 12340)

> Source: offsets.txt, sections "Object Funcs 1/2/3"

## Hooked Functions

### FrameScript_Execute
- **Address**: `0x00819210`
- **Convention**: `__cdecl`
- **Signature**: `void(const char* code, const char* filename, int unused)`
- **Status**: HOOKED (hooks.cpp:31)
- **Notes**: Executes Lua code in WoW's scripting engine

---

## All Functions

| Address | Name |
|---------|------|
| `0x00401B00` | FrameScript__Reload |
| `0x004181B0` | FrameScript_RegisterFunction |
| `0x0044E2C0` | FrameScript_toboolean |
| `0x007225E0` | FrameScript__GetLocalizedText |
| `0x00750400` | FrameScript__SignalCombatLogEvent |
| `0x00765FF0` | RegisterHandlers_1 |
| `0x00771B80` | SErrRegisterHandler |
| `0x008167E0` | FrameScript__FillScriptMethodTable |
| `0x00817FD0` | FrameScript_UnregisterFunction |
| `0x00818010` | FrameScript_GetVariable |
| `0x00819210` | FrameScript_Execute |
| `0x00819D40` | FrameScript_GetText |
| `0x0081AC90` | FrameScript_SignalEvent |
| `0x0084DBD0` | FrameScript_GetTop |
| `0x0084DBF0` | FrameScript__SetTop |
| `0x0084DF20` | FrameScript__IsNumber |
| `0x0084DFE0` | FrameScript_equal |
| `0x0084E030` | FrameScript_ToNumber |
| `0x0084E070` | FrameScript_tointeger |
| `0x0084E0E0` | FrameScript_ToLString |
| `0x0084E150` | FrameScript_objlen |
| `0x0084E1C0` | FrameScript_tocfunction |
| `0x0084E1F0` | FrameScript_tothread |
| `0x0084E210` | FrameScript_touserdata |
| `0x0084E2D0` | FrameScript_pushinteger |
| `0x0084E350` | FrameScript__PushString |
| `0x0084E400` | FrameScript_pushcclosure |
| `0x0084E4D0` | FrameScript_pushboolean |
| `0x0084E590` | FrameScript__FindTable |
| `0x0084E900` | FrameScript_setfield |
| `0x0084EC50` | FrameScript_PCall |
| `0x0084F3B0` | FrameScript_getfield |
| `0x0084F860` | FrameScript_Load |
