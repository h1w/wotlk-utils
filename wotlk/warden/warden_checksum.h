#pragma once
#include <cstdint>

namespace warden_checksum {

// Build Warden CHEAT_CHECKS_RESULT checksum:
// SHA1(data) → reinterpret as 5 x uint32_t (native LE) → XOR-fold → uint32_t
uint32_t BuildChecksum(const uint8_t* data, uint32_t length);

// Validate checksum against expected value
bool ValidateChecksum(uint32_t expected, const uint8_t* data, uint32_t length);

} // namespace warden_checksum
