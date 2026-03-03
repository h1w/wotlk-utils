#pragma once
#include <cstdint>

namespace hooks {

// Инициализация MinHook и установка всех хуков.
// Вызывать после logger::Initialize() и из потока, где уже доступна консоль.
bool Initialize();

// Снятие всех хуков и деинициализация MinHook.
// Вызывать перед logger::Shutdown().
void Shutdown();

// Try to extract the HASH_REQUEST seed from the current (already decrypted)
// SMSG CDataStore.  Called from SpoofHashResultIfNeeded when the seed hasn't
// been stored yet — the HASH_RESULT encryption fires BEFORE PostHandler
// stores the seed, but the CDataStore has already been decrypted in-place.
bool TryExtractHashSeedFromCurrentPacket(uint8_t outSeed[16]);

// Access the original (unhooked) FrameScript_Execute trampoline.
// Used by lua_bridge to execute Lua without triggering our logging hook.
using FrameScriptExecuteFn = void(__cdecl*)(const char*, const char*, int);
FrameScriptExecuteFn GetOriginalFrameScriptExecute();

// Enable/disable NtGetContextThread hook that hides debug registers (DR0-DR7).
// Called by warden_rc4_hook when hardware breakpoints are set/cleared.
// The hook is created (but disabled) in Initialize(); these toggle it on demand.
void EnableContextGuard();
void DisableContextGuard();

} // namespace hooks
