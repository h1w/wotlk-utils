#include "mpq_archive.h"

// vcpkg StormLib is built WITHOUT Unicode — include BEFORE windows.h
// so TCHAR resolves to char (matching the DLL's ANSI exports).
#pragma push_macro("UNICODE")
#pragma push_macro("_UNICODE")
#undef UNICODE
#undef _UNICODE
#define WIN32_LEAN_AND_MEAN
#include <StormLib.h>
#pragma pop_macro("_UNICODE")
#pragma pop_macro("UNICODE")

// Now include windows.h with Unicode restored (for GetLastError, etc.)
#include <windows.h>

#include <glog/logging.h>
#include <filesystem>
#include <algorithm>

namespace mapedit {

// MPQ priority: highest-numbered patches first, then base archives.
static const char* kMpqOrder[] = {
    "patch-3.MPQ",
    "patch-2.MPQ",
    "patch.MPQ",
    "lichking.MPQ",
    "expansion.MPQ",
    "common-2.MPQ",
    "common.MPQ",
};

bool MpqArchiveSet::Open(const std::string& dataDir) {
    Close();

    namespace fs = std::filesystem;
    if (!fs::is_directory(dataDir)) {
        LOG(ERROR) << "[MpqArchive] Not a directory: " << dataDir;
        return false;
    }

    // Auto-detect Data subfolder: if selected dir has a "Data" child with MPQ files, use it.
    std::string actualDir = dataDir;
    fs::path dataSubDir = fs::path(dataDir) / "Data";
    if (fs::is_directory(dataSubDir)) {
        // Check if any .MPQ/.mpq files exist in the Data subfolder
        for (const auto& entry : fs::directory_iterator(dataSubDir)) {
            if (!entry.is_regular_file()) continue;
            std::string ext = entry.path().extension().string();
            // Case-insensitive .mpq check
            if (ext.size() == 4 &&
                (ext[1] == 'M' || ext[1] == 'm') &&
                (ext[2] == 'P' || ext[2] == 'p') &&
                (ext[3] == 'Q' || ext[3] == 'q')) {
                actualDir = dataSubDir.string();
                LOG(INFO) << "[MpqArchive] Auto-detected Data subfolder: " << actualDir;
                break;
            }
        }
    }
    m_dataDir = actualDir;

    // Log all files in the directory for diagnostics
    for (const auto& entry : fs::directory_iterator(actualDir)) {
        if (!entry.is_regular_file()) continue;
        std::string ext = entry.path().extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
        if (ext == ".mpq")
            LOG(INFO) << "[MpqArchive] Found MPQ: " << entry.path().filename().string();
    }

    int opened = 0;

    // Try each known MPQ name
    for (const char* name : kMpqOrder) {
        std::string path = (fs::path(actualDir) / name).string();
        if (!fs::exists(path)) {
            LOG(INFO) << "[MpqArchive] Not found: " << path;
            continue;
        }

        HANDLE hMpq = nullptr;
        if (SFileOpenArchive(path.c_str(), 0, MPQ_OPEN_READ_ONLY, &hMpq)) {
            m_archives.push_back(hMpq);
            opened++;
            LOG(INFO) << "[MpqArchive] Opened: " << name;
        } else {
            DWORD err = GetLastError();
            LOG(WARNING) << "[MpqArchive] Failed to open " << path
                         << " — error " << err;
        }
    }

    // Also try locale directories (e.g., enUS/, ruRU/)
    for (const auto& entry : fs::directory_iterator(actualDir)) {
        if (!entry.is_directory()) continue;
        std::string dirName = entry.path().filename().string();
        if (dirName.size() != 4) continue; // locale dirs are 4 chars (enUS, ruRU, etc.)

        // Try locale MPQs in priority order
        const char* localeMpqs[] = {
            "patch-%s-3.MPQ", "patch-%s-2.MPQ", "patch-%s.MPQ", "locale-%s.MPQ"
        };
        for (const char* fmt : localeMpqs) {
            char name[64];
            snprintf(name, sizeof(name), fmt, dirName.c_str());
            std::string path = (entry.path() / name).string();
            if (!fs::exists(path)) continue;

            HANDLE hMpq = nullptr;
            if (SFileOpenArchive(path.c_str(), 0, MPQ_OPEN_READ_ONLY, &hMpq)) {
                m_archives.push_back(hMpq);
                opened++;
                LOG(INFO) << "[MpqArchive] Opened: " << dirName << "/" << name;
            } else {
                DWORD err = GetLastError();
                LOG(WARNING) << "[MpqArchive] Failed to open " << path
                             << " — error " << err;
            }
        }
    }

    LOG(INFO) << "[MpqArchive] Opened " << opened << " archives from " << actualDir;
    return opened > 0;
}

void MpqArchiveSet::Close() {
    for (void* h : m_archives)
        SFileCloseArchive(static_cast<HANDLE>(h));
    m_archives.clear();
}

std::vector<uint8_t> MpqArchiveSet::ReadFile(const std::string& internalPath) const {
    for (void* h : m_archives) {
        HANDLE hFile = nullptr;
        if (!SFileOpenFileEx(static_cast<HANDLE>(h), internalPath.c_str(),
                             SFILE_OPEN_FROM_MPQ, &hFile))
            continue;

        DWORD fileSize = SFileGetFileSize(hFile, nullptr);
        if (fileSize == SFILE_INVALID_SIZE || fileSize == 0) {
            SFileCloseFile(hFile);
            continue;
        }

        std::vector<uint8_t> data(fileSize);
        DWORD bytesRead = 0;
        if (SFileReadFile(hFile, data.data(), fileSize, &bytesRead, nullptr) &&
            bytesRead == fileSize) {
            SFileCloseFile(hFile);
            return data;
        }

        SFileCloseFile(hFile);
    }
    return {};
}

bool MpqArchiveSet::HasFile(const std::string& internalPath) const {
    for (void* h : m_archives) {
        if (SFileHasFile(static_cast<HANDLE>(h), internalPath.c_str()))
            return true;
    }
    return false;
}

std::vector<std::string> MpqArchiveSet::ListFiles(const std::string& mask, int maxResults) const {
    std::vector<std::string> results;
    SFILE_FIND_DATA findData;
    for (void* h : m_archives) {
        HANDLE hFind = SFileFindFirstFile(static_cast<HANDLE>(h), mask.c_str(), &findData, nullptr);
        if (!hFind) continue;
        do {
            results.push_back(findData.cFileName);
            if (static_cast<int>(results.size()) >= maxResults) break;
        } while (SFileFindNextFile(hFind, &findData));
        SFileFindClose(hFind);
        if (static_cast<int>(results.size()) >= maxResults) break;
    }
    return results;
}

} // namespace mapedit
