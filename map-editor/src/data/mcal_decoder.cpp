#include "mcal_decoder.h"

#include <algorithm>
#include <cstring>

namespace mapedit {

AlphaFormat GetAlphaFormat(uint32_t mphdFlags, uint32_t mclyFlags) {
    if (mclyFlags & 0x200)
        return AlphaFormat::CompressedRle;
    if (mphdFlags & 0x004)
        return AlphaFormat::Uncompressed8Bit;
    return AlphaFormat::Packed4Bit;
}

static void Decode4Bit(const uint8_t* src, size_t srcSize, uint8_t out[64 * 64]) {
    // 4-bit packed: 2048 bytes -> 4096 values, low nibble first.
    // Values 0-15 expanded to 0-255 via val | (val << 4).
    std::memset(out, 0, 4096);
    size_t bytes = (std::min)(srcSize, static_cast<size_t>(2048));
    for (size_t i = 0; i < bytes; ++i) {
        uint8_t lo = src[i] & 0x0F;
        uint8_t hi = (src[i] >> 4) & 0x0F;
        out[i * 2]     = lo | (lo << 4);
        out[i * 2 + 1] = hi | (hi << 4);
    }
}

static void Decode8Bit(const uint8_t* src, size_t srcSize, uint8_t out[64 * 64]) {
    size_t bytes = (std::min)(srcSize, static_cast<size_t>(4096));
    std::memcpy(out, src, bytes);
    if (bytes < 4096)
        std::memset(out + bytes, 0, 4096 - bytes);
}

static void DecodeRle(const uint8_t* src, size_t srcSize, uint8_t out[64 * 64]) {
    // Control byte: bit7 = fill/copy, bits 6-0 = count.
    // Fill: repeat next byte 'count' times.
    // Copy: copy 'count' literal bytes.
    // Output capped at 4096 bytes.
    size_t inPos = 0;
    size_t outPos = 0;

    while (outPos < 4096 && inPos < srcSize) {
        uint8_t control = src[inPos++];
        int count = control & 0x7F;

        if (control & 0x80) {
            // Fill mode
            if (inPos >= srcSize) break;
            uint8_t value = src[inPos++];
            size_t end = (std::min)(outPos + static_cast<size_t>(count),
                                    static_cast<size_t>(4096));
            std::memset(out + outPos, value, end - outPos);
            outPos = end;
        } else {
            // Copy mode
            for (int i = 0; i < count && outPos < 4096 && inPos < srcSize; ++i) {
                out[outPos++] = src[inPos++];
            }
        }
    }

    // Zero-fill remainder
    if (outPos < 4096)
        std::memset(out + outPos, 0, 4096 - outPos);
}

void DecodeAlpha(AlphaFormat fmt, const uint8_t* src, size_t srcSize, uint8_t out[64 * 64]) {
    switch (fmt) {
    case AlphaFormat::Packed4Bit:
        Decode4Bit(src, srcSize, out);
        break;
    case AlphaFormat::Uncompressed8Bit:
        Decode8Bit(src, srcSize, out);
        break;
    case AlphaFormat::CompressedRle:
        DecodeRle(src, srcSize, out);
        break;
    }
}

} // namespace mapedit
