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

} // namespace hooks
