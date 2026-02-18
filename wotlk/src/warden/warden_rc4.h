#pragma once
#include <cstdint>
#include <cstddef>

namespace warden_rc4 {

// Reset state on MODULE_USE (new module loading)
void Reset();

// Scan process memory for RC4 S-box permutations (call after MODULE_INITIALIZE)
bool ScanForRC4States();

// Are there any S-box candidates to work with?
bool HasCandidates();

// Clone all found RC4 states (call in WardenPreHandler before original runs)
void CloneAllStates();

// Decrypt CMSG payload by trying all cloned states.
// Returns true if decryption succeeded (plaintext[0] is a valid Warden client opcode).
// On success, writes result to outBuf.
bool DecryptCmsg(const uint8_t* encrypted, size_t len,
                 uint8_t* outBuf, size_t outBufSize);

// Has the encrypt state been identified?
bool HasEncryptState();

} // namespace warden_rc4
