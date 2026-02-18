#include "peb_unlink.h"

#include <glog/logging.h>

#include <cstdint>
#include <vector>

namespace peb_unlink {

// ============================================================================
// Undocumented PEB/LDR structures (x86, stable since Windows XP)
// ============================================================================

struct UNICODE_STRING_32 {
    USHORT Length;
    USHORT MaximumLength;
    PWSTR  Buffer;
};

struct PEB_LDR_DATA_32 {
    ULONG      Length;
    BOOLEAN    Initialized;
    PVOID      SsHandle;
    LIST_ENTRY InLoadOrderModuleList;
    LIST_ENTRY InMemoryOrderModuleList;
    LIST_ENTRY InInitializationOrderModuleList;
};

struct LDR_DATA_TABLE_ENTRY_32 {
    LIST_ENTRY     InLoadOrderLinks;
    LIST_ENTRY     InMemoryOrderLinks;
    LIST_ENTRY     InInitializationOrderLinks;
    PVOID          DllBase;
    PVOID          EntryPoint;
    ULONG          SizeOfImage;
    UNICODE_STRING_32 FullDllName;
    UNICODE_STRING_32 BaseDllName;
    // ... more fields we don't need
};

struct PEB_32 {
    BYTE          Reserved1[2];
    BYTE          BeingDebugged;
    BYTE          Reserved2[1];
    PVOID         Reserved3[2];
    PEB_LDR_DATA_32* Ldr;
    // ... more fields we don't need
};

// ============================================================================
// Saved state for relinking
// ============================================================================

struct SavedLinks {
    LDR_DATA_TABLE_ENTRY_32* entry;
    // InLoadOrderLinks neighbors
    LIST_ENTRY* loadFlink;
    LIST_ENTRY* loadBlink;
    // InMemoryOrderLinks neighbors
    LIST_ENTRY* memFlink;
    LIST_ENTRY* memBlink;
    // InInitializationOrderLinks neighbors
    LIST_ENTRY* initFlink;
    LIST_ENTRY* initBlink;
};

static std::vector<SavedLinks> g_saved;
static bool g_unlinked = false;

// ============================================================================
// Internal helpers
// ============================================================================

static PEB_32* GetPEB()
{
    return reinterpret_cast<PEB_32*>(__readfsdword(0x30));
}

// Find LDR_DATA_TABLE_ENTRY for a given DllBase by walking InLoadOrderModuleList.
static LDR_DATA_TABLE_ENTRY_32* FindLdrEntry(PEB_LDR_DATA_32* ldr, PVOID dllBase)
{
    LIST_ENTRY* head = &ldr->InLoadOrderModuleList;
    LIST_ENTRY* cur  = head->Flink;
    while (cur != head) {
        auto* entry = CONTAINING_RECORD(cur, LDR_DATA_TABLE_ENTRY_32, InLoadOrderLinks);
        if (entry->DllBase == dllBase)
            return entry;
        cur = cur->Flink;
    }
    return nullptr;
}

// Unlink a single LIST_ENTRY from its doubly-linked list.
static void UnlinkEntry(LIST_ENTRY* entry)
{
    entry->Blink->Flink = entry->Flink;
    entry->Flink->Blink = entry->Blink;
}

// Re-insert a LIST_ENTRY between savedBlink and savedFlink.
static void RelinkEntry(LIST_ENTRY* entry, LIST_ENTRY* savedFlink, LIST_ENTRY* savedBlink)
{
    entry->Flink = savedFlink;
    entry->Blink = savedBlink;
    savedBlink->Flink = entry;
    savedFlink->Blink = entry;
}

static bool UnlinkModule(PEB_LDR_DATA_32* ldr, HMODULE hMod, const char* name)
{
    auto* entry = FindLdrEntry(ldr, static_cast<PVOID>(hMod));
    if (!entry) {
        LOG(WARNING) << "[PEB] Could not find LDR entry for " << name;
        return false;
    }

    SavedLinks saved = {};
    saved.entry = entry;
    saved.loadFlink = entry->InLoadOrderLinks.Flink;
    saved.loadBlink = entry->InLoadOrderLinks.Blink;
    saved.memFlink  = entry->InMemoryOrderLinks.Flink;
    saved.memBlink  = entry->InMemoryOrderLinks.Blink;
    saved.initFlink = entry->InInitializationOrderLinks.Flink;
    saved.initBlink = entry->InInitializationOrderLinks.Blink;

    UnlinkEntry(&entry->InLoadOrderLinks);
    UnlinkEntry(&entry->InMemoryOrderLinks);
    UnlinkEntry(&entry->InInitializationOrderLinks);

    g_saved.push_back(saved);

    LOG(INFO) << "[PEB] Unlinked " << name << " (base=0x"
              << std::hex << reinterpret_cast<uintptr_t>(hMod) << ")";
    return true;
}

// ============================================================================
// Public API
// ============================================================================

bool UnlinkAll(HMODULE hModule)
{
    if (g_unlinked) {
        LOG(WARNING) << "[PEB] Already unlinked";
        return true;
    }

    PEB_32* peb = GetPEB();
    if (!peb || !peb->Ldr) {
        LOG(ERROR) << "[PEB] Failed to get PEB/LDR";
        return false;
    }

    PEB_LDR_DATA_32* ldr = peb->Ldr;
    g_saved.clear();

    // Collect module handles for our DLL + dependencies.
    // GetModuleHandleA returns NULL if the DLL is not loaded (e.g. gflags vs gflags_debug).
    struct ModuleInfo {
        HMODULE handle;
        const char* name;
    };

    ModuleInfo candidates[] = {
        { hModule,                              "wotlk.dll" },
        { GetModuleHandleA("glog.dll"),         "glog.dll" },
        { GetModuleHandleA("gflags_debug.dll"), "gflags_debug.dll" },
        { GetModuleHandleA("gflags.dll"),       "gflags.dll" },
    };

    int count = 0;
    for (const auto& mod : candidates) {
        if (mod.handle) {
            if (UnlinkModule(ldr, mod.handle, mod.name))
                count++;
        }
    }

    if (count > 0) {
        g_unlinked = true;
        LOG(INFO) << "[PEB] Unlinking complete: " << std::dec << count << " module(s) hidden";
        return true;
    }

    LOG(ERROR) << "[PEB] Failed to unlink any modules";
    return false;
}

bool RelinkAll()
{
    if (!g_unlinked || g_saved.empty()) {
        LOG(WARNING) << "[PEB] Nothing to relink";
        return true;
    }

    // Relink in reverse order (last unlinked first)
    for (auto it = g_saved.rbegin(); it != g_saved.rend(); ++it) {
        const auto& saved = *it;
        RelinkEntry(&saved.entry->InInitializationOrderLinks, saved.initFlink, saved.initBlink);
        RelinkEntry(&saved.entry->InMemoryOrderLinks, saved.memFlink, saved.memBlink);
        RelinkEntry(&saved.entry->InLoadOrderLinks, saved.loadFlink, saved.loadBlink);

        LOG(INFO) << "[PEB] Relinked module (base=0x"
                  << std::hex << reinterpret_cast<uintptr_t>(saved.entry->DllBase) << ")";
    }

    int count = static_cast<int>(g_saved.size());
    g_saved.clear();
    g_unlinked = false;

    LOG(INFO) << "[PEB] Re-linking complete: " << std::dec << count << " module(s) restored";
    return true;
}

bool IsUnlinked()
{
    return g_unlinked;
}

} // namespace peb_unlink
