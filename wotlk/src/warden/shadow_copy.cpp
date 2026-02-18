#include "shadow_copy.h"

#define NOMINMAX
#include <Windows.h>
#include <glog/logging.h>

#include <cstring>

namespace shadow {

static HANDLE g_fileMapping = nullptr;
static LPVOID g_mappedView  = nullptr;

static uintptr_t g_imageBase  = 0;
static uintptr_t g_textRVA    = 0;
static uint32_t  g_textVSize  = 0;
static uint32_t  g_textRawOff = 0;

bool Initialize()
{
    wchar_t exePath[MAX_PATH];
    if (!GetModuleFileNameW(nullptr, exePath, MAX_PATH)) {
        LOG(ERROR) << "[SHADOW] GetModuleFileName failed: " << GetLastError();
        return false;
    }

    HANDLE hFile = CreateFileW(exePath, GENERIC_READ, FILE_SHARE_READ,
                               nullptr, OPEN_EXISTING, 0, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) {
        LOG(ERROR) << "[SHADOW] CreateFile failed: " << GetLastError();
        return false;
    }

    g_fileMapping = CreateFileMappingW(hFile, nullptr, PAGE_READONLY, 0, 0, nullptr);
    CloseHandle(hFile);

    if (!g_fileMapping) {
        LOG(ERROR) << "[SHADOW] CreateFileMapping failed: " << GetLastError();
        return false;
    }

    g_mappedView = MapViewOfFile(g_fileMapping, FILE_MAP_READ, 0, 0, 0);
    if (!g_mappedView) {
        LOG(ERROR) << "[SHADOW] MapViewOfFile failed: " << GetLastError();
        CloseHandle(g_fileMapping);
        g_fileMapping = nullptr;
        return false;
    }

    // Parse PE headers
    auto dosHeader = reinterpret_cast<IMAGE_DOS_HEADER*>(g_mappedView);
    if (dosHeader->e_magic != IMAGE_DOS_SIGNATURE) {
        LOG(ERROR) << "[SHADOW] Invalid DOS signature";
        Shutdown();
        return false;
    }

    if (dosHeader->e_lfanew < sizeof(IMAGE_DOS_HEADER) ||
        dosHeader->e_lfanew > 0x10000) {
        LOG(ERROR) << "[SHADOW] Invalid e_lfanew: " << dosHeader->e_lfanew;
        Shutdown();
        return false;
    }

    auto ntHeaders = reinterpret_cast<IMAGE_NT_HEADERS32*>(
        reinterpret_cast<uint8_t*>(g_mappedView) + dosHeader->e_lfanew);
    if (ntHeaders->Signature != IMAGE_NT_SIGNATURE) {
        LOG(ERROR) << "[SHADOW] Invalid NT signature";
        Shutdown();
        return false;
    }

    g_imageBase = ntHeaders->OptionalHeader.ImageBase;

    // Find .text section
    auto section = IMAGE_FIRST_SECTION(ntHeaders);
    bool found = false;
    for (WORD i = 0; i < ntHeaders->FileHeader.NumberOfSections; ++i, ++section) {
        if (std::memcmp(section->Name, ".text", 5) == 0) {
            g_textRVA    = section->VirtualAddress;
            g_textVSize  = section->Misc.VirtualSize;
            g_textRawOff = section->PointerToRawData;
            found = true;
            break;
        }
    }

    if (!found) {
        LOG(ERROR) << "[SHADOW] .text section not found";
        Shutdown();
        return false;
    }

    LOG(INFO) << "[SHADOW] Shadow copy initialized: imageBase=0x"
              << std::hex << g_imageBase
              << " .text RVA=0x" << g_textRVA
              << " size=0x" << g_textVSize
              << " rawOff=0x" << g_textRawOff;

    return true;
}

void Shutdown()
{
    if (g_mappedView) {
        UnmapViewOfFile(g_mappedView);
        g_mappedView = nullptr;
    }
    if (g_fileMapping) {
        CloseHandle(g_fileMapping);
        g_fileMapping = nullptr;
    }
    g_textRVA = 0;
    g_textVSize = 0;
    g_textRawOff = 0;

    LOG(INFO) << "[SHADOW] Shadow copy shut down";
}

bool IsInTextSection(uintptr_t runtimeAddr)
{
    if (!g_mappedView || g_textVSize == 0)
        return false;

    uintptr_t textStart = g_imageBase + g_textRVA;
    uintptr_t textEnd   = textStart + g_textVSize;

    return (runtimeAddr >= textStart && runtimeAddr < textEnd);
}

bool GetCleanBytes(uintptr_t runtimeAddr, uint8_t* out, size_t len)
{
    if (!g_mappedView || !out || len == 0)
        return false;

    uintptr_t textStart = g_imageBase + g_textRVA;
    uintptr_t textEnd   = textStart + g_textVSize;

    if (runtimeAddr < textStart || runtimeAddr >= textEnd)
        return false;

    if (len > textEnd - runtimeAddr)
        return false;

    uintptr_t offsetInText = runtimeAddr - textStart;
    uintptr_t fileOffset   = g_textRawOff + offsetInText;

    std::memcpy(out, reinterpret_cast<uint8_t*>(g_mappedView) + fileOffset, len);
    return true;
}

} // namespace shadow
