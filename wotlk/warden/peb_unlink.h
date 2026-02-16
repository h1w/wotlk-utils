#pragma once

#define NOMINMAX
#include <Windows.h>

namespace peb_unlink {

// Unlink our DLL + its dynamic dependencies (glog, gflags) from all 3 PEB.Ldr lists.
// After this, CreateToolhelp32Snapshot(TH32CS_SNAPMODULE) won't enumerate them.
// Must be called AFTER all initialization that uses GetModuleHandle/GetModuleFileName.
bool UnlinkAll(HMODULE hModule);

// Re-insert all previously unlinked modules into PEB.Ldr lists.
// Must be called BEFORE FreeLibraryAndExitThread so LdrUnloadDll can find the module.
bool RelinkAll();

// Returns true if modules are currently unlinked.
bool IsUnlinked();

} // namespace peb_unlink
