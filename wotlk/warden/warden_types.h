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
    WARDEN_CMSG_MEM_CHECKS_RESULT   = 0x04,
    WARDEN_CMSG_HASH_RESULT         = 0x05,
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
    WARDEN_CHECK_PAGE_A   = 0x22,  // 34  — Page check variant A (25 data bytes)
    WARDEN_CHECK_PAGE_B   = 0x47,  // 71  — Page check variant B (25 data bytes)
    WARDEN_CHECK_PROC     = 0x69,  // 105 — Process/addr check (25 data bytes)
    WARDEN_CHECK_MEM      = 0x8E,  // 142 — Memory hash check (27 data bytes)
    WARDEN_CHECK_MPQ      = 0x91,  // 145 — MPQ/addr check (25 data bytes)
    WARDEN_CHECK_MODULE   = 0xB3,  // 179 — Module check (1 data byte)
    WARDEN_CHECK_DRIVER   = 0xD8,  // 216 — Driver check (1 data byte)
    WARDEN_CHECK_LUA      = 0xDB,  // 219 — Lua/string check (2 data bytes)
};

// Data bytes consumed from stream per check type (AFTER the type byte)
inline size_t WardenCheckDataSize(uint8_t type)
{
    switch (type) {
    case WARDEN_CHECK_TIMING:  return 0;
    case WARDEN_CHECK_PAGE_A:
    case WARDEN_CHECK_PAGE_B:
    case WARDEN_CHECK_PROC:
    case WARDEN_CHECK_MPQ:     return 25;  // 4 addr + 20 SHA1 + 1 len
    case WARDEN_CHECK_MEM:     return 27;  // 4 addr + 20 SHA1 + 1 + 1 + 1 len
    case WARDEN_CHECK_MODULE:
    case WARDEN_CHECK_DRIVER:  return 1;
    case WARDEN_CHECK_LUA:     return 2;
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
    default:                              return "UNKNOWN";
    }
}
