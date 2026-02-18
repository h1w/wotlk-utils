#pragma once
#include <cstdint>
#include <cstddef>

namespace warden_rc4_hook {

// Clear state on MODULE_USE (before new module loads)
void Reset();

// Scan runtime module memory for RC4 PRGA function and install hook.
// Returns true if hook was successfully installed.
bool Install(uintptr_t moduleBase, size_t moduleSize);

// Remove hook before module unload
void Remove();

// Is the internal RC4 hook currently active?
bool IsActive();

// Retrieve captured CMSG plaintext (one-shot: clears buffer after retrieval).
// Returns true if plaintext was available.
bool ConsumePlaintext(uint8_t* out, size_t outSize, size_t* outLen);

// Cleanup resources (call before DLL unload, after Remove)
void Cleanup();

} // namespace warden_rc4_hook
