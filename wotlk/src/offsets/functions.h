#pragma once
// =============================================================================
// WoW 3.3.5a (build 12340) — Centralized Offsets & Addresses
// =============================================================================

#include <cstdint>

namespace offsets {

// =========================================================================
// Function addresses
// =========================================================================
namespace fn {

// Scripting
inline constexpr uintptr_t FrameScript_Execute     = 0x00819210;
inline constexpr uintptr_t FrameScript_RegisterFunc = 0x00817F90;
inline constexpr uintptr_t FrameScript_GetText      = 0x00819D40;

// Network
inline constexpr uintptr_t SendPacket               = 0x00632B50;

// Encryption
inline constexpr uintptr_t ARC4_Process             = 0x00774EA0;
inline constexpr uintptr_t GenSecureRandom          = 0x0086D640;

// Warden
inline constexpr uintptr_t WardenHandler            = 0x007DA850;

// ObjectManager
inline constexpr uintptr_t ClntObjMgrObjectPtr      = 0x004D4DB0;

// Unit
inline constexpr uintptr_t UnitReaction             = 0x007251C0;
inline constexpr uintptr_t HasAuraBySpellId         = 0x007282A0;

// Spells
inline constexpr uintptr_t IsSpellKnown             = 0x0053C5B0;
inline constexpr uintptr_t GetSpellCooldown         = 0x00807980;

// UI
inline constexpr uintptr_t CGGameUI_Target          = 0x00524BF0;

// Movement
inline constexpr uintptr_t ClickToMove              = 0x00727400;
inline constexpr uintptr_t SetFacing                = 0x0072EA50;
inline constexpr uintptr_t MoveForwardStart         = 0x005FC200;
inline constexpr uintptr_t MoveForwardStop          = 0x005FC240;
inline constexpr uintptr_t MoveBackwardStart        = 0x005FC280;
inline constexpr uintptr_t MoveBackwardStop         = 0x005FC2C0;
inline constexpr uintptr_t TurnLeftStart            = 0x005FC300;
inline constexpr uintptr_t TurnLeftStop             = 0x005FC340;
inline constexpr uintptr_t TurnRightStart           = 0x005FC380;
inline constexpr uintptr_t TurnRightStop            = 0x005FC3C0;
inline constexpr uintptr_t StrafeLeftStart          = 0x005FC400;
inline constexpr uintptr_t StrafeLeftStop           = 0x005FC440;
inline constexpr uintptr_t StrafeRightStart         = 0x005FC480;
inline constexpr uintptr_t StrafeRightStop          = 0x005FC4C0;

// World
inline constexpr uintptr_t TraceLine                = 0x007A3B70;

} // namespace fn

// =========================================================================
// Packet opcodes
// =========================================================================
namespace opcodes {

inline constexpr uint32_t SMSG_WARDEN_DATA = 0x2E6;
inline constexpr uint32_t CMSG_WARDEN_DATA = 0x2E7;

} // namespace opcodes

// =========================================================================
// PE layout
// =========================================================================
namespace pe {

inline constexpr uintptr_t ImageBase  = 0x00400000;
inline constexpr uintptr_t TextRVA    = 0x00001000;
inline constexpr size_t    TextSize   = 0x005DD3B3;
inline constexpr uintptr_t TextStart  = ImageBase + TextRVA;
inline constexpr uintptr_t TextEnd    = TextStart + TextSize;

} // namespace pe

// =========================================================================
// ObjectManager struct offsets
// =========================================================================
namespace objmgr {

inline constexpr uintptr_t CurMgrPointer    = 0x00C79CE0; // -> ClientConnection
inline constexpr uintptr_t ObjMgrOffset     = 0x2ED0;     // ClientConnection -> ObjMgr
inline constexpr uintptr_t FirstObject      = 0xAC;       // ObjMgr -> first WowObject
inline constexpr uintptr_t NextObject       = 0x3C;       // WowObject -> next WowObject
inline constexpr uintptr_t ObjectType       = 0x14;       // WowObject -> type enum
inline constexpr uintptr_t ObjectGUID       = 0x30;       // WowObject -> GUID (uint64)
inline constexpr uintptr_t Descriptor       = 0x08;       // WowObject -> descriptor base ptr
inline constexpr uintptr_t LocalPlayerGUID  = 0xC0;       // ObjMgr -> local player GUID

} // namespace objmgr

// =========================================================================
// Global memory addresses
// =========================================================================
namespace globals {

inline constexpr uintptr_t PlayerName          = 0x00C79D18; // char[48]
inline constexpr uintptr_t TargetGUID          = 0x00BD07B0; // uint64_t
inline constexpr uintptr_t MouseOverGUID       = 0x00BD07A0; // uint64_t
inline constexpr uintptr_t ComboPoints         = 0x00BD084D; // uint8_t
inline constexpr uintptr_t ZoneText            = 0x00BD0780; // char*
inline constexpr uintptr_t SubZoneText         = 0x00BD0784; // char*
inline constexpr uintptr_t MapId               = 0x00AB63BC; // uint32_t
inline constexpr uintptr_t ZoneId              = 0x00BD080C; // uint32_t
inline constexpr uintptr_t RealmName           = 0x00C79B9E; // char[64]
inline constexpr uintptr_t IsLoadingOrConnecting = 0x00B6AA38; // uint32_t (0=in-game)
inline constexpr uintptr_t GameState           = 0x00B6A9C0; // uint32_t
inline constexpr uintptr_t CameraPointer       = 0x00B7436C; // -> camera struct

} // namespace globals

// =========================================================================
// VTable indices (WowObject vtable, multiply by 4 for byte offset)
// =========================================================================
namespace vtable {

inline constexpr int GetPosition = 12;
inline constexpr int GetFacing   = 14;
inline constexpr int GetName     = 54;

} // namespace vtable

// =========================================================================
// Descriptor field indices (field * 4 = byte offset from descriptor base)
// =========================================================================
namespace fields {

// Object
inline constexpr int OBJECT_GUID             = 0x00;
inline constexpr int OBJECT_TYPE             = 0x02;
inline constexpr int OBJECT_ENTRY            = 0x03;
inline constexpr int OBJECT_SCALE            = 0x04;

// Unit
inline constexpr int UNIT_CHARM             = 0x06;
inline constexpr int UNIT_SUMMON            = 0x08;
inline constexpr int UNIT_CREATEDBY         = 0x0C;
inline constexpr int UNIT_TARGET            = 0x12;
inline constexpr int UNIT_HEALTH            = 0x18;
inline constexpr int UNIT_POWER1            = 0x19;
inline constexpr int UNIT_POWER2            = 0x1A;
inline constexpr int UNIT_POWER3            = 0x1B;
inline constexpr int UNIT_POWER4            = 0x1C;
inline constexpr int UNIT_POWER5            = 0x1D;
inline constexpr int UNIT_POWER6            = 0x1E;
inline constexpr int UNIT_POWER7            = 0x1F;
inline constexpr int UNIT_MAXHEALTH         = 0x20;
inline constexpr int UNIT_MAXPOWER1         = 0x21;
inline constexpr int UNIT_MAXPOWER2         = 0x22;
inline constexpr int UNIT_MAXPOWER3         = 0x23;
inline constexpr int UNIT_MAXPOWER4         = 0x24;
inline constexpr int UNIT_MAXPOWER5         = 0x25;
inline constexpr int UNIT_MAXPOWER6         = 0x26;
inline constexpr int UNIT_MAXPOWER7         = 0x27;
inline constexpr int UNIT_LEVEL             = 0x36;
inline constexpr int UNIT_FACTIONTEMPLATE   = 0x37;
inline constexpr int UNIT_FLAGS             = 0x3C;
inline constexpr int UNIT_FLAGS_2           = 0x3D;
inline constexpr int UNIT_DISPLAYID         = 0x40;
inline constexpr int UNIT_NATIVEDISPLAYID   = 0x41;
inline constexpr int UNIT_MOUNTDISPLAYID    = 0x42;
inline constexpr int UNIT_CREATURETYPE      = 0x49;
inline constexpr int UNIT_STAT0             = 0x60;
inline constexpr int UNIT_STAT1             = 0x61;
inline constexpr int UNIT_STAT2             = 0x62;
inline constexpr int UNIT_STAT3             = 0x63;
inline constexpr int UNIT_STAT4             = 0x64;

// Player
inline constexpr int PLAYER_XP             = 0x3B6;
inline constexpr int PLAYER_NEXT_LEVEL_XP  = 0x3B7;
inline constexpr int PLAYER_COINAGE        = 0x492;

} // namespace fields

// =========================================================================
// Unit struct offsets (direct, not descriptors)
// =========================================================================
namespace unit {

inline constexpr uintptr_t CastingSpellId    = 0xC08;
inline constexpr uintptr_t ChanneledSpellId  = 0xC20;
inline constexpr uintptr_t NameOffset1       = 0x964; // -> name struct
inline constexpr uintptr_t NameOffset2       = 0x05C; // name struct -> const char*

} // namespace unit

// =========================================================================
// ClickToMove action types
// =========================================================================
namespace ctm {

inline constexpr int Move     = 0x04;
inline constexpr int Interact = 0x06;
inline constexpr int Loot     = 0x07;
inline constexpr int Attack   = 0x0A;
inline constexpr int Stop     = 0x0D;

} // namespace ctm

} // namespace offsets
