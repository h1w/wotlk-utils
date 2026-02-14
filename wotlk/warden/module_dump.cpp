#include "module_dump.h"

#define NOMINMAX
#include <Windows.h>
#include <glog/logging.h>

#include "../third_party/miniz.h"

#include <vector>
#include <string>
#include <cstring>
#include <sstream>
#include <iomanip>
#include <fstream>

namespace module_dump {

static uint8_t  g_hash[16] = {};
static uint8_t  g_key[16]  = {};
static uint32_t g_expectedSize = 0;
static std::vector<uint8_t> g_buffer;
static bool g_capturing = false;

static std::vector<uint8_t> g_decompressedModule;

static std::string HexStr(const uint8_t* data, size_t len)
{
    std::ostringstream oss;
    for (size_t i = 0; i < len; ++i)
        oss << std::hex << std::uppercase << std::setfill('0') << std::setw(2)
            << static_cast<int>(data[i]);
    return oss.str();
}

static void RC4Transform(uint8_t* data, size_t len, const uint8_t* key, size_t keyLen)
{
    uint8_t S[256];
    for (int i = 0; i < 256; i++)
        S[i] = static_cast<uint8_t>(i);

    uint8_t j = 0;
    for (int i = 0; i < 256; i++) {
        j = j + S[i] + key[i % keyLen];
        uint8_t tmp = S[i]; S[i] = S[j]; S[j] = tmp;
    }

    uint8_t ii = 0;
    j = 0;
    for (size_t n = 0; n < len; n++) {
        ii++;
        j += S[ii];
        uint8_t tmp = S[ii]; S[ii] = S[j]; S[j] = tmp;
        data[n] ^= S[static_cast<uint8_t>(S[ii] + S[j])];
    }
}

// Decompress zlib data from decrypted module buffer.
// Format: [uint32_t decompressed_size][zlib_data...]
// Returns true if decompression succeeded and g_decompressedModule is populated.
static bool DecompressModule(const uint8_t* decrypted, size_t decryptedLen)
{
    if (decryptedLen < 5) {
        LOG(WARNING) << "[WARDEN] Decrypted module too small for decompression header";
        return false;
    }

    uint32_t decompSize;
    std::memcpy(&decompSize, decrypted, 4);

    if (decompSize == 0 || decompSize > 4 * 1024 * 1024) {
        LOG(WARNING) << "[WARDEN] Suspicious decompressed size: " << decompSize;
        return false;
    }

    // Verify zlib magic (78 xx)
    if (decrypted[4] != 0x78) {
        LOG(WARNING) << "[WARDEN] No zlib magic at offset 4 (got 0x"
                     << std::hex << std::setfill('0') << std::setw(2)
                     << static_cast<int>(decrypted[4]) << ")";
        return false;
    }

    g_decompressedModule.resize(decompSize);
    mz_ulong destLen = decompSize;
    int ret = mz_uncompress(g_decompressedModule.data(), &destLen,
                            decrypted + 4, static_cast<mz_ulong>(decryptedLen - 4));

    if (ret != MZ_OK) {
        LOG(ERROR) << "[WARDEN] zlib decompress failed: " << mz_error(ret)
                   << " (code " << ret << ")";
        g_decompressedModule.clear();
        return false;
    }

    g_decompressedModule.resize(destLen);
    LOG(INFO) << "[WARDEN] Decompressed module: " << destLen << " bytes"
              << " (from " << (decryptedLen - 4) << " compressed)";
    return true;
}

void Reset()
{
    g_buffer.clear();
    g_buffer.shrink_to_fit();
    g_decompressedModule.clear();
    g_decompressedModule.shrink_to_fit();
    std::memset(g_hash, 0, sizeof(g_hash));
    std::memset(g_key, 0, sizeof(g_key));
    g_expectedSize = 0;
    g_capturing = false;
    LOG(INFO) << "[WARDEN] Module dump state reset";
}

void OnModuleUse(const uint8_t* data, size_t len)
{
    // Format: [0x00] [16-byte MD5] [16-byte RC4 key] [4-byte LE compressed size]
    if (len < 37) return;

    std::memcpy(g_hash, data + 1, 16);
    std::memcpy(g_key,  data + 17, 16);
    std::memcpy(&g_expectedSize, data + 33, 4);

    g_buffer.clear();
    g_buffer.reserve(g_expectedSize);
    g_capturing = true;

    LOG(INFO) << "[WARDEN] Module capture started: hash="
              << HexStr(g_hash, 16) << " expectedSize=" << std::dec << g_expectedSize;
}

void OnModuleCache(const uint8_t* data, size_t len)
{
    if (!g_capturing || len < 3) return;

    // Format: [0x01] [uint16 LE chunk_size] [chunk_data...]
    uint16_t chunkSize;
    std::memcpy(&chunkSize, data + 1, 2);

    if (static_cast<size_t>(3) + chunkSize > len) {
        LOG(WARNING) << "[WARDEN] Module capture: chunk truncated, chunkSize="
                     << chunkSize << " available=" << (len - 3);
        return;
    }

    g_buffer.insert(g_buffer.end(), data + 3, data + 3 + chunkSize);
}

void OnModuleInitialize(const uint8_t* data, size_t len)
{
    bool wasCapturing = g_capturing;
    g_capturing = false;

    if (g_buffer.empty()) {
        // No MODULE_CACHE packets received — server sent a cached module.
        // g_decompressedModule may already be populated via TryLoadFromCache.
        if (HasDecompressedModule()) {
            LOG(INFO) << "[WARDEN] Module Initialize with cached decompressed module ("
                      << g_decompressedModule.size() << " bytes)";
        } else if (wasCapturing) {
            LOG(WARNING) << "[WARDEN] Module capture: no MODULE_CACHE data received"
                         << " (server reused cached module, hash="
                         << HexStr(g_hash, 16) << ")";
        } else {
            LOG(WARNING) << "[WARDEN] Module capture: not capturing and no cached module";
        }
        g_buffer.shrink_to_fit();
        return;
    }

    LOG(INFO) << "[WARDEN] Module capture complete: "
              << g_buffer.size() << "/" << g_expectedSize << " bytes";

    CreateDirectoryA("warden_dumps", nullptr);

    std::string hashStr = HexStr(g_hash, 16);

    // 1. Write raw (encrypted + compressed) data
    {
        std::string path = "warden_dumps\\warden_" + hashStr + "_encrypted.bin";
        std::ofstream f(path, std::ios::binary);
        if (f) {
            f.write(reinterpret_cast<const char*>(g_buffer.data()), g_buffer.size());
            LOG(INFO) << "[WARDEN] Written: " << path << " (" << g_buffer.size() << " bytes)";
        } else {
            LOG(ERROR) << "[WARDEN] Failed to write: " << path;
        }
    }

    // 2. Decrypt with RC4 and write
    std::vector<uint8_t> decrypted(g_buffer);
    RC4Transform(decrypted.data(), decrypted.size(), g_key, 16);

    {
        std::string path = "warden_dumps\\warden_" + hashStr + "_decrypted.bin";
        std::ofstream f(path, std::ios::binary);
        if (f) {
            f.write(reinterpret_cast<const char*>(decrypted.data()), decrypted.size());
            LOG(INFO) << "[WARDEN] Written: " << path << " (" << decrypted.size() << " bytes)";
        } else {
            LOG(ERROR) << "[WARDEN] Failed to write: " << path;
        }
    }

    // 3. Decompress with zlib and write
    if (DecompressModule(decrypted.data(), decrypted.size())) {
        std::string path = "warden_dumps\\warden_" + hashStr + "_decompressed.bin";
        std::ofstream f(path, std::ios::binary);
        if (f) {
            f.write(reinterpret_cast<const char*>(g_decompressedModule.data()),
                    g_decompressedModule.size());
            LOG(INFO) << "[WARDEN] Written: " << path
                      << " (" << g_decompressedModule.size() << " bytes)";
        } else {
            LOG(ERROR) << "[WARDEN] Failed to write: " << path;
        }
    }

    // 4. Write metadata + MODULE_INITIALIZE payload
    {
        std::string path = "warden_dumps\\warden_" + hashStr + "_meta.txt";
        std::ofstream f(path);
        if (f) {
            f << "Warden Module Dump\n"
              << "MD5 hash: " << hashStr << "\n"
              << "RC4 key:  " << HexStr(g_key, 16) << "\n"
              << "Size:     " << g_buffer.size() << " bytes (encrypted+compressed)\n"
              << "Expected: " << g_expectedSize << " bytes\n";
            if (HasDecompressedModule())
                f << "Decompressed: " << g_decompressedModule.size() << " bytes\n";
            f << "\nMODULE_INITIALIZE payload (" << len << " bytes):\n"
              << HexStr(data, len) << "\n";
            LOG(INFO) << "[WARDEN] Written: " << path;
        }
    }

    g_buffer.clear();
    g_buffer.shrink_to_fit();
}

bool TryLoadFromCache(const uint8_t* hash16)
{
    std::string hashStr = HexStr(hash16, 16);

    // Try decompressed file first (fastest)
    {
        std::string path = "warden_dumps\\warden_" + hashStr + "_decompressed.bin";
        std::ifstream f(path, std::ios::binary | std::ios::ate);
        if (f) {
            auto fileSize = f.tellg();
            if (fileSize > 0 && fileSize < 4 * 1024 * 1024) {
                g_decompressedModule.resize(static_cast<size_t>(fileSize));
                f.seekg(0);
                f.read(reinterpret_cast<char*>(g_decompressedModule.data()),
                       g_decompressedModule.size());
                if (f) {
                    LOG(INFO) << "[WARDEN] Loaded cached decompressed module: " << path
                              << " (" << g_decompressedModule.size() << " bytes)";
                    return true;
                }
                g_decompressedModule.clear();
            }
        }
    }

    // Fallback: try decrypted file (need to decompress)
    {
        std::string path = "warden_dumps\\warden_" + hashStr + "_decrypted.bin";
        std::ifstream f(path, std::ios::binary | std::ios::ate);
        if (f) {
            auto fileSize = f.tellg();
            if (fileSize > 4 && fileSize < 4 * 1024 * 1024) {
                std::vector<uint8_t> decrypted(static_cast<size_t>(fileSize));
                f.seekg(0);
                f.read(reinterpret_cast<char*>(decrypted.data()), decrypted.size());
                if (f && DecompressModule(decrypted.data(), decrypted.size())) {
                    LOG(INFO) << "[WARDEN] Loaded cached decrypted module and decompressed: "
                              << path;
                    // Save decompressed for next time
                    CreateDirectoryA("warden_dumps", nullptr);
                    std::string decompPath = "warden_dumps\\warden_" + hashStr
                                           + "_decompressed.bin";
                    std::ofstream out(decompPath, std::ios::binary);
                    if (out) {
                        out.write(reinterpret_cast<const char*>(g_decompressedModule.data()),
                                  g_decompressedModule.size());
                        LOG(INFO) << "[WARDEN] Written: " << decompPath
                                  << " (" << g_decompressedModule.size() << " bytes)";
                    }
                    return true;
                }
            }
        }
    }

    LOG(INFO) << "[WARDEN] No cached module found for hash=" << hashStr;
    return false;
}

bool HasDecompressedModule()
{
    return !g_decompressedModule.empty();
}

const uint8_t* GetDecompressedModule(size_t& outLen)
{
    if (g_decompressedModule.empty()) {
        outLen = 0;
        return nullptr;
    }
    outLen = g_decompressedModule.size();
    return g_decompressedModule.data();
}

} // namespace module_dump
