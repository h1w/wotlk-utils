#pragma once

namespace hooks {

// Инициализация MinHook и установка всех хуков.
// Вызывать после logger::Initialize() и из потока, где уже доступна консоль.
bool Initialize();

// Снятие всех хуков и деинициализация MinHook.
// Вызывать перед logger::Shutdown().
void Shutdown();

} // namespace hooks
