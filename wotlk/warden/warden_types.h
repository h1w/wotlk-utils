#pragma once
#include <cstdint>

// ---------------------------------------------------------------------------
// Warden Server -> Client команды (первый байт SMSG_WARDEN_DATA)
// ---------------------------------------------------------------------------
enum WardenServerOpcode : uint8_t {
    WARDEN_SMSG_MODULE_USE            = 0x00,
    WARDEN_SMSG_MODULE_CACHE          = 0x01,
    WARDEN_SMSG_CHEAT_CHECKS_REQUEST  = 0x02,
    WARDEN_SMSG_MODULE_INITIALIZE     = 0x03,
    WARDEN_SMSG_HASH_REQUEST          = 0x05,  // Module integrity SHA1 challenge
};

// ---------------------------------------------------------------------------
// Warden Client -> Server команды (первый байт CMSG_WARDEN_DATA payload)
// NOTE: При перехвате на уровне ClientServices::SendPacket payload уже
// зашифрован Warden RC4 — эти значения видны только до шифрования.
// ---------------------------------------------------------------------------
enum WardenClientOpcode : uint8_t {
    WARDEN_CMSG_MODULE_MISSING      = 0x00,
    WARDEN_CMSG_MODULE_OK           = 0x01,
    WARDEN_CMSG_CHEAT_CHECKS_RESULT = 0x02,
    WARDEN_CMSG_MEM_CHECKS_RESULT   = 0x03,
    WARDEN_CMSG_HASH_RESULT         = 0x04,
    WARDEN_CMSG_MODULE_FAILED       = 0x05,
};

// ---------------------------------------------------------------------------
// Типы проверок внутри CHEAT_CHECKS_REQUEST
//
// ВАЖНО: Эти ID специфичны для Warden-модуля 7C4ABC97B86494A2D91820785F3A1C87.
// Другие модули используют ДРУГИЕ значения для тех же типов проверок.
// Например, модуль DA3BF29EB72099327FDFA6ED7322C35E использует:
//   TIMING=0x74, LUA=0x70, остальные — TBD.
//
// В пакете тип XOR-ится с xorByte (последний байт пакета).
// Данные проверок (адреса, хеши) НЕ XOR-ятся — только байт типа.
//
// Для модуль-агностичного обнаружения MEM_CHECK используется сканирование
// по паттерну адресов хуков (ScanForHookAddresses в hooks.cpp).
//
// Определены через reverse engineering модуля: диспетчер в RVA 0x2613.
// ---------------------------------------------------------------------------
enum WardenCheckType : uint8_t {
    WARDEN_CHECK_TIMING   = 0x1F,  // 31  — Timing check (0 data bytes)
    WARDEN_CHECK_PAGE_A   = 0x22,  // 34  — Page check variant A (29 data bytes: seed(4)+SHA1(20)+addr(4)+readLen(1))
    WARDEN_CHECK_PAGE_B   = 0x47,  // 71  — Page check variant B (29 data bytes: seed(4)+SHA1(20)+addr(4)+readLen(1))
    WARDEN_CHECK_PROC     = 0x69,  // 105 — Process/addr check (31 data bytes: seed(4)+SHA1(20)+modIdx(1)+procIdx(1)+addr(4)+readLen(1))
    WARDEN_CHECK_MEM      = 0x8E,  // 142 — Memory hash check (6 data bytes: unk(1)+addr(4)+readLen(1))
    WARDEN_CHECK_MPQ      = 0x91,  // 145 — MPQ file check (1 data byte: stringIndex(1))
    WARDEN_CHECK_MODULE   = 0xB3,  // 179 — Module check (24 data bytes: seed(4)+SHA1(20))
    WARDEN_CHECK_DRIVER   = 0xD8,  // 216 — Driver check (25 data bytes: seed(4)+SHA1(20)+stringIndex(1))
    WARDEN_CHECK_LUA      = 0xDB,  // 219 — Lua/string check (1 data byte: stringIndex(1))
};

// Data bytes consumed from stream per check type (AFTER the type byte)
// Sizes verified from AzerothCore source and independent RE research.
inline size_t WardenCheckDataSize(uint8_t type)
{
    switch (type) {
    case WARDEN_CHECK_TIMING:  return 0;   // empty
    case WARDEN_CHECK_PAGE_A:
    case WARDEN_CHECK_PAGE_B:  return 29;  // seed(4) + SHA1(20) + addr(4) + readLen(1)
    case WARDEN_CHECK_PROC:    return 31;  // seed(4) + SHA1(20) + modIdx(1) + procIdx(1) + addr(4) + readLen(1)
    case WARDEN_CHECK_MEM:     return 6;   // unk(1) + addr(4) + readLen(1)
    case WARDEN_CHECK_MPQ:     return 1;   // stringIndex(1)
    case WARDEN_CHECK_MODULE:  return 24;  // seed(4) + SHA1(20)
    case WARDEN_CHECK_DRIVER:  return 25;  // seed(4) + SHA1(20) + stringIndex(1)
    case WARDEN_CHECK_LUA:     return 1;   // stringIndex(1)
    default:                   return 0;   // unknown — can't skip
    }
}

inline bool IsKnownWardenCheckType(uint8_t type)
{
    return WardenCheckDataSize(type) > 0 || type == WARDEN_CHECK_TIMING;
}

inline const char* WardenServerOpcodeToString(uint8_t op)
{
    switch (op) {
    case WARDEN_SMSG_MODULE_USE:           return "MODULE_USE";
    case WARDEN_SMSG_MODULE_CACHE:         return "MODULE_CACHE";
    case WARDEN_SMSG_CHEAT_CHECKS_REQUEST: return "CHEAT_CHECKS_REQUEST";
    case WARDEN_SMSG_MODULE_INITIALIZE:    return "MODULE_INITIALIZE";
    case WARDEN_SMSG_HASH_REQUEST:         return "HASH_REQUEST";
    default:                               return "UNKNOWN";
    }
}

inline const char* WardenCheckTypeToString(uint8_t type)
{
    switch (type) {
    case WARDEN_CHECK_TIMING:  return "TIMING";
    case WARDEN_CHECK_PAGE_A:  return "PAGE_A";
    case WARDEN_CHECK_PAGE_B:  return "PAGE_B";
    case WARDEN_CHECK_PROC:    return "PROC";
    case WARDEN_CHECK_MEM:     return "MEM_CHECK";
    case WARDEN_CHECK_MPQ:     return "MPQ";
    case WARDEN_CHECK_MODULE:  return "MODULE";
    case WARDEN_CHECK_DRIVER:  return "DRIVER";
    case WARDEN_CHECK_LUA:     return "LUA_EVAL";
    default:                   return "UNKNOWN";
    }
}

inline const char* WardenClientOpcodeToString(uint8_t op)
{
    switch (op) {
    case WARDEN_CMSG_MODULE_MISSING:      return "MODULE_MISSING";
    case WARDEN_CMSG_MODULE_OK:           return "MODULE_OK";
    case WARDEN_CMSG_CHEAT_CHECKS_RESULT: return "CHEAT_CHECKS_RESULT";
    case WARDEN_CMSG_MEM_CHECKS_RESULT:   return "MEM_CHECKS_RESULT";
    case WARDEN_CMSG_HASH_RESULT:         return "HASH_RESULT";
    case WARDEN_CMSG_MODULE_FAILED:       return "MODULE_FAILED";
    default:                              return "UNKNOWN";
    }
}
