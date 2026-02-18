#include "mpq_cache.h"

#include <glog/logging.h>

#include <array>
#include <algorithm>
#include <cctype>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>
#include <unordered_map>

namespace mpq_cache {

// ---------------------------------------------------------------------------
// Case-insensitive key for MPQ paths (e.g. "World\Maps\..." vs "world\maps\...")
// ---------------------------------------------------------------------------

static std::string ToLower(const std::string& s)
{
    std::string out = s;
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

static bool HexToByte(const char* hex, uint8_t& out)
{
    unsigned int val = 0;
    for (int i = 0; i < 2; ++i) {
        char c = hex[i];
        val <<= 4;
        if (c >= '0' && c <= '9')      val |= (c - '0');
        else if (c >= 'A' && c <= 'F') val |= (c - 'A' + 10);
        else if (c >= 'a' && c <= 'f') val |= (c - 'a' + 10);
        else return false;
    }
    out = static_cast<uint8_t>(val);
    return true;
}

static std::string BytesToHex(const uint8_t* data, size_t len)
{
    std::ostringstream oss;
    for (size_t i = 0; i < len; ++i)
        oss << std::hex << std::uppercase << std::setfill('0') << std::setw(2)
            << static_cast<int>(data[i]);
    return oss.str();
}

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

using Hash20 = std::array<uint8_t, 20>;
static std::unordered_map<std::string, Hash20> g_cache;  // key = lowered MPQ path
static std::string g_filePath;  // full path to mpq_hashes.txt
static bool g_dirty = false;
static bool g_initialized = false;
static CRITICAL_SECTION g_lock;
static bool g_lockInit = false;

// ---------------------------------------------------------------------------
// File I/O
// ---------------------------------------------------------------------------

static bool ParseLine(const std::string& line, std::string& outKey, Hash20& outHash)
{
    // Skip empty lines and comments
    if (line.empty() || line[0] == '#')
        return false;

    size_t eq = line.find('=');
    if (eq == std::string::npos || eq == 0)
        return false;

    std::string key = line.substr(0, eq);
    std::string hexStr = line.substr(eq + 1);

    // Trim whitespace
    while (!key.empty() && key.back() == ' ') key.pop_back();
    while (!hexStr.empty() && hexStr[0] == ' ') hexStr.erase(hexStr.begin());
    while (!hexStr.empty() && (hexStr.back() == ' ' || hexStr.back() == '\r' || hexStr.back() == '\n'))
        hexStr.pop_back();

    if (hexStr.size() != 40)
        return false;

    Hash20 hash;
    for (int i = 0; i < 20; ++i) {
        if (!HexToByte(hexStr.c_str() + i * 2, hash[i]))
            return false;
    }

    outKey = key;
    outHash = hash;
    return true;
}

static void LoadFromFile()
{
    g_cache.clear();

    std::ifstream file(g_filePath);
    if (!file.is_open()) {
        LOG(INFO) << "[MPQ_CACHE] No hash file found at " << g_filePath << " (will create on capture)";
        return;
    }

    std::string line;
    int count = 0;
    while (std::getline(file, line)) {
        std::string key;
        Hash20 hash;
        if (ParseLine(line, key, hash)) {
            g_cache[ToLower(key)] = hash;
            ++count;
        }
    }

    LOG(INFO) << "[MPQ_CACHE] Loaded " << count << " hash(es) from " << g_filePath;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

bool Initialize(HMODULE hModule)
{
    if (!g_lockInit) {
        InitializeCriticalSection(&g_lock);
        g_lockInit = true;
    }

    // Get DLL directory
    wchar_t pathW[MAX_PATH] = {};
    DWORD len = GetModuleFileNameW(hModule, pathW, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) {
        LOG(ERROR) << "[MPQ_CACHE] GetModuleFileNameW failed";
        return false;
    }

    // Convert to narrow string and strip filename
    char pathA[MAX_PATH] = {};
    WideCharToMultiByte(CP_UTF8, 0, pathW, -1, pathA, MAX_PATH, nullptr, nullptr);

    std::string dir(pathA);
    size_t lastSlash = dir.find_last_of("\\/");
    if (lastSlash != std::string::npos)
        dir = dir.substr(0, lastSlash + 1);

    g_filePath = dir + "mpq_hashes.txt";
    LoadFromFile();
    g_initialized = true;
    return true;
}

void Shutdown()
{
    if (!g_initialized)
        return;

    EnterCriticalSection(&g_lock);
    if (g_dirty)
        SaveToFile();
    g_cache.clear();
    g_initialized = false;
    LeaveCriticalSection(&g_lock);

    DeleteCriticalSection(&g_lock);
    g_lockInit = false;
}

const uint8_t* LookupHash(const std::string& filename)
{
    if (!g_initialized || filename.empty())
        return nullptr;

    EnterCriticalSection(&g_lock);
    auto it = g_cache.find(ToLower(filename));
    // Safe to return pointer: unordered_map guarantees reference stability on insert.
    const uint8_t* result = (it != g_cache.end()) ? it->second.data() : nullptr;
    LeaveCriticalSection(&g_lock);
    return result;
}

void CaptureHash(const std::string& filename, const uint8_t* sha1)
{
    if (!g_initialized || filename.empty() || !sha1)
        return;

    std::string key = ToLower(filename);

    EnterCriticalSection(&g_lock);
    if (g_cache.count(key)) {
        LeaveCriticalSection(&g_lock);
        return;  // already cached
    }

    Hash20 hash;
    std::memcpy(hash.data(), sha1, 20);
    g_cache[key] = hash;
    g_dirty = true;
    LeaveCriticalSection(&g_lock);

    LOG(INFO) << "[MPQ_CACHE] Captured hash for \"" << filename
              << "\": " << BytesToHex(sha1, 20);
}

void SaveToFile()
{
    if (g_filePath.empty())
        return;

    std::ofstream file(g_filePath, std::ios::trunc);
    if (!file.is_open()) {
        LOG(ERROR) << "[MPQ_CACHE] Failed to open " << g_filePath << " for writing";
        return;
    }

    file << "# Warden MPQ hash cache (auto-generated)\n";
    for (const auto& kv : g_cache) {
        // Write original-cased key would be nice, but we only have lowered keys.
        // MPQ paths are case-insensitive so this is fine.
        file << kv.first << "=" << BytesToHex(kv.second.data(), 20) << "\n";
    }

    g_dirty = false;
    LOG(INFO) << "[MPQ_CACHE] Saved " << g_cache.size() << " hash(es) to " << g_filePath;
}

} // namespace mpq_cache
