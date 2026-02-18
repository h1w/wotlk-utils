#pragma once
// =============================================================================
// WoW 3.3.5a (build 12340) — Function Addresses
//
// Documentation: wotlk/docs/reference/offsets/functions/
// =============================================================================

#include <cstdint>

namespace offsets {

// === Scripting (see functions/scripting.md) ===
// void __cdecl(const char* code, const char* filename, int unused)
inline constexpr uintptr_t FrameScript_Execute      = 0x00819210;
inline constexpr uintptr_t FrameScript_RegisterFunc  = 0x00817F90;
inline constexpr uintptr_t FrameScript_GetText       = 0x00819D40;

// === Network (see functions/network.md) ===
// naked hook — CDataStore* is stk1 after pushad/pushfd
inline constexpr uintptr_t SendPacket                = 0x00632B50;

// === Encryption (see functions/encryption.md) ===
// __thiscall — ECX=ARC4*, stk1=data, stk2=len
// Session header cipher, NOT Warden payload cipher
inline constexpr uintptr_t ARC4_Process              = 0x00774EA0;
inline constexpr uintptr_t GenSecureRandom           = 0x0086D640;

// === Warden (see functions/warden.md) ===
// naked hook — non-standard stack, return-address hijack
inline constexpr uintptr_t WardenHandler             = 0x007DA850;

// === Packet Opcodes ===
inline constexpr uint32_t SMSG_WARDEN_DATA           = 0x2E6;
inline constexpr uint32_t CMSG_WARDEN_DATA           = 0x2E7;

// === PE Layout (see data_structures/cdatastore.md) ===
inline constexpr uintptr_t ImageBase                 = 0x00400000;
inline constexpr uintptr_t TextRVA                   = 0x00001000;
inline constexpr size_t    TextSize                   = 0x005DD3B3;
inline constexpr uintptr_t TextStart                 = ImageBase + TextRVA;
inline constexpr uintptr_t TextEnd                   = TextStart + TextSize;

} // namespace offsets
