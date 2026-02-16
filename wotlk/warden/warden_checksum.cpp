#include "warden_checksum.h"

#define NOMINMAX
#include <Windows.h>
#include <wincrypt.h>

namespace warden_checksum {

uint32_t BuildChecksum(const uint8_t* data, uint32_t length)
{
    HCRYPTPROV hProv = 0;
    HCRYPTHASH hHash = 0;
    uint32_t checksum = 0;

    if (!CryptAcquireContextW(&hProv, nullptr, nullptr, PROV_RSA_FULL,
                              CRYPT_VERIFYCONTEXT))
        return 0;

    if (!CryptCreateHash(hProv, CALG_SHA1, 0, 0, &hHash)) {
        CryptReleaseContext(hProv, 0);
        return 0;
    }

    if (!CryptHashData(hHash, data, length, 0)) {
        CryptDestroyHash(hHash);
        CryptReleaseContext(hProv, 0);
        return 0;
    }

    uint8_t digest[20];
    DWORD digestLen = 20;
    if (!CryptGetHashParam(hHash, HP_HASHVAL, digest, &digestLen, 0)) {
        CryptDestroyHash(hHash);
        CryptReleaseContext(hProv, 0);
        return 0;
    }

    CryptDestroyHash(hHash);
    CryptReleaseContext(hProv, 0);

    // XOR-fold 20-byte SHA1 as 5 x uint32_t (native little-endian)
    const uint32_t* words = reinterpret_cast<const uint32_t*>(digest);
    checksum = words[0] ^ words[1] ^ words[2] ^ words[3] ^ words[4];

    return checksum;
}

bool ValidateChecksum(uint32_t expected, const uint8_t* data, uint32_t length)
{
    return BuildChecksum(data, length) == expected;
}

bool ComputeSHA1(const uint8_t* data, uint32_t length, uint8_t outDigest[20])
{
    HCRYPTPROV hProv = 0;
    HCRYPTHASH hHash = 0;

    if (!CryptAcquireContextW(&hProv, nullptr, nullptr, PROV_RSA_FULL,
                              CRYPT_VERIFYCONTEXT))
        return false;

    if (!CryptCreateHash(hProv, CALG_SHA1, 0, 0, &hHash)) {
        CryptReleaseContext(hProv, 0);
        return false;
    }

    if (!CryptHashData(hHash, data, length, 0)) {
        CryptDestroyHash(hHash);
        CryptReleaseContext(hProv, 0);
        return false;
    }

    DWORD digestLen = 20;
    if (!CryptGetHashParam(hHash, HP_HASHVAL, outDigest, &digestLen, 0)) {
        CryptDestroyHash(hHash);
        CryptReleaseContext(hProv, 0);
        return false;
    }

    CryptDestroyHash(hHash);
    CryptReleaseContext(hProv, 0);
    return true;
}

} // namespace warden_checksum
