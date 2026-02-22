#include "blp_decoder.h"
#include <cstring>
#include <algorithm>

namespace mapedit {

// ---- DXT helpers ----

static void DecodeRgb565(uint16_t c, uint8_t& r, uint8_t& g, uint8_t& b) {
    r = static_cast<uint8_t>(((c >> 11) & 0x1F) * 255 / 31);
    g = static_cast<uint8_t>(((c >> 5)  & 0x3F) * 255 / 63);
    b = static_cast<uint8_t>((c & 0x1F) * 255 / 31);
}

static void DecodeDxt1Block(const uint8_t* block, uint8_t* out, int stride) {
    uint16_t c0, c1;
    std::memcpy(&c0, block, 2);
    std::memcpy(&c1, block + 2, 2);
    uint32_t indices;
    std::memcpy(&indices, block + 4, 4);

    uint8_t colors[4][4]; // [index][B,G,R,A]
    DecodeRgb565(c0, colors[0][2], colors[0][1], colors[0][0]);
    colors[0][3] = 255;
    DecodeRgb565(c1, colors[1][2], colors[1][1], colors[1][0]);
    colors[1][3] = 255;

    if (c0 > c1) {
        for (int i = 0; i < 3; i++) {
            colors[2][i] = static_cast<uint8_t>((2 * colors[0][i] + colors[1][i]) / 3);
            colors[3][i] = static_cast<uint8_t>((colors[0][i] + 2 * colors[1][i]) / 3);
        }
        colors[2][3] = 255;
        colors[3][3] = 255;
    } else {
        for (int i = 0; i < 3; i++)
            colors[2][i] = static_cast<uint8_t>((colors[0][i] + colors[1][i]) / 2);
        colors[2][3] = 255;
        colors[3][0] = colors[3][1] = colors[3][2] = colors[3][3] = 0;
    }

    for (int y = 0; y < 4; y++) {
        for (int x = 0; x < 4; x++) {
            int idx = (indices >> (2 * (y * 4 + x))) & 0x3;
            uint8_t* p = out + y * stride + x * 4;
            p[0] = colors[idx][0];
            p[1] = colors[idx][1];
            p[2] = colors[idx][2];
            p[3] = colors[idx][3];
        }
    }
}

static void DecodeDxt3Block(const uint8_t* block, uint8_t* out, int stride) {
    // First 8 bytes: explicit alpha (4 bits per pixel)
    const uint8_t* alphaBlock = block;
    // Next 8 bytes: color block (same as DXT1 4-color mode)
    DecodeDxt1Block(block + 8, out, stride);

    // Apply explicit alpha
    for (int y = 0; y < 4; y++) {
        uint16_t row;
        std::memcpy(&row, alphaBlock + y * 2, 2);
        for (int x = 0; x < 4; x++) {
            int a4 = (row >> (x * 4)) & 0xF;
            out[y * stride + x * 4 + 3] = static_cast<uint8_t>(a4 * 255 / 15);
        }
    }
}

static void DecodeDxt5Block(const uint8_t* block, uint8_t* out, int stride) {
    // First 2 bytes: alpha endpoints
    uint8_t a0 = block[0], a1 = block[1];
    // Next 6 bytes: 4x4 3-bit alpha indices
    // Last 8 bytes: color block
    DecodeDxt1Block(block + 8, out, stride);

    // Build alpha palette
    uint8_t alphas[8];
    alphas[0] = a0;
    alphas[1] = a1;
    if (a0 > a1) {
        for (int i = 0; i < 6; i++)
            alphas[2 + i] = static_cast<uint8_t>(((6 - i) * a0 + (1 + i) * a1) / 7);
    } else {
        for (int i = 0; i < 4; i++)
            alphas[2 + i] = static_cast<uint8_t>(((4 - i) * a0 + (1 + i) * a1) / 5);
        alphas[6] = 0;
        alphas[7] = 255;
    }

    // Decode 3-bit indices (48 bits = 6 bytes, starting at block+2)
    uint64_t bits = 0;
    for (int i = 0; i < 6; i++)
        bits |= static_cast<uint64_t>(block[2 + i]) << (8 * i);

    for (int y = 0; y < 4; y++) {
        for (int x = 0; x < 4; x++) {
            int idx = static_cast<int>((bits >> (3 * (y * 4 + x))) & 0x7);
            out[y * stride + x * 4 + 3] = alphas[idx];
        }
    }
}

// ---- BLP2 format ----

#pragma pack(push, 1)
struct Blp2Header {
    char     magic[4];      // "BLP2"
    uint32_t type;          // always 1
    uint8_t  compression;   // 1=paletted, 2=DXT, 3=uncompressed
    uint8_t  alphaDepth;    // 0,1,4,8
    uint8_t  alphaType;     // 0,1,7 (DXT subtype selector)
    uint8_t  hasMips;
    uint32_t width;
    uint32_t height;
    uint32_t mipOffsets[16];
    uint32_t mipSizes[16];
    uint32_t palette[256];  // BGRA palette (only for compression=1)
};
#pragma pack(pop)

bool DecodeBlp(const uint8_t* data, size_t size, BlpImage& out) {
    if (size < sizeof(Blp2Header)) return false;

    const auto* hdr = reinterpret_cast<const Blp2Header*>(data);
    if (std::memcmp(hdr->magic, "BLP2", 4) != 0) return false;

    int w = static_cast<int>(hdr->width);
    int h = static_cast<int>(hdr->height);
    if (w <= 0 || h <= 0 || w > 4096 || h > 4096) return false;

    // Only load mip level 0
    uint32_t mipOff  = hdr->mipOffsets[0];
    uint32_t mipSize = hdr->mipSizes[0];
    if (mipOff == 0 || mipSize == 0) return false;
    if (static_cast<size_t>(mipOff + mipSize) > size) return false;
    const uint8_t* mipData = data + mipOff;

    out.width  = w;
    out.height = h;
    out.bgra.resize(w * h * 4);
    int stride = w * 4;

    if (hdr->compression == 1) {
        // Paletted: each byte is an index into the 256-entry BGRA palette.
        // Alpha is stored separately after the index data.
        if (mipSize < static_cast<uint32_t>(w * h)) return false;
        const uint8_t* indices = mipData;
        const uint8_t* alphaData = mipData + w * h;
        size_t alphaAvailable = (mipOff + mipSize > static_cast<uint32_t>(mipOff + w * h))
                                ? mipSize - w * h : 0;

        for (int y = 0; y < h; y++) {
            for (int x = 0; x < w; x++) {
                int pixelIdx = y * w + x;
                uint8_t palIdx = indices[pixelIdx];
                uint32_t color = hdr->palette[palIdx]; // BGRA
                uint8_t* dst = out.bgra.data() + (y * w + x) * 4;
                dst[0] = static_cast<uint8_t>((color >>  0) & 0xFF); // B
                dst[1] = static_cast<uint8_t>((color >>  8) & 0xFF); // G
                dst[2] = static_cast<uint8_t>((color >> 16) & 0xFF); // R

                // Alpha depends on alphaDepth
                if (hdr->alphaDepth == 0) {
                    dst[3] = 255;
                } else if (hdr->alphaDepth == 1 && alphaAvailable > 0) {
                    int byteIdx = pixelIdx / 8;
                    int bitIdx  = pixelIdx % 8;
                    if (static_cast<size_t>(byteIdx) < alphaAvailable)
                        dst[3] = ((alphaData[byteIdx] >> bitIdx) & 1) ? 255 : 0;
                    else
                        dst[3] = 255;
                } else if (hdr->alphaDepth == 4 && alphaAvailable > 0) {
                    int byteIdx = pixelIdx / 2;
                    if (static_cast<size_t>(byteIdx) < alphaAvailable) {
                        int a4 = (pixelIdx & 1) ? (alphaData[byteIdx] >> 4)
                                                 : (alphaData[byteIdx] & 0xF);
                        dst[3] = static_cast<uint8_t>(a4 * 255 / 15);
                    } else {
                        dst[3] = 255;
                    }
                } else if (hdr->alphaDepth == 8 && alphaAvailable > 0) {
                    if (static_cast<size_t>(pixelIdx) < alphaAvailable)
                        dst[3] = alphaData[pixelIdx];
                    else
                        dst[3] = 255;
                } else {
                    dst[3] = 255;
                }
            }
        }
    } else if (hdr->compression == 2) {
        // DXT compressed
        int bw = (w + 3) / 4;
        int bh = (h + 3) / 4;

        // Determine DXT variant
        int dxtType; // 1, 3, or 5
        if (hdr->alphaType == 0 && hdr->alphaDepth <= 1) dxtType = 1;
        else if (hdr->alphaType == 1) dxtType = 3;
        else dxtType = 5; // alphaType == 7 or others

        int blockSize = (dxtType == 1) ? 8 : 16;
        if (mipSize < static_cast<uint32_t>(bw * bh * blockSize)) return false;

        // Decode into a padded buffer (multiple of 4)
        int pw = bw * 4;
        int ph = bh * 4;
        std::vector<uint8_t> padded(pw * ph * 4, 0);

        for (int by = 0; by < bh; by++) {
            for (int bx = 0; bx < bw; bx++) {
                const uint8_t* block = mipData + (by * bw + bx) * blockSize;
                uint8_t* dst = padded.data() + (by * 4 * pw + bx * 4) * 4;

                switch (dxtType) {
                case 1: DecodeDxt1Block(block, dst, pw * 4); break;
                case 3: DecodeDxt3Block(block, dst, pw * 4); break;
                case 5: DecodeDxt5Block(block, dst, pw * 4); break;
                }
            }
        }

        // Copy from padded to actual size
        for (int y = 0; y < h; y++)
            std::memcpy(out.bgra.data() + y * stride,
                        padded.data() + y * pw * 4, stride);
    } else if (hdr->compression == 3) {
        // Uncompressed BGRA
        if (mipSize < static_cast<uint32_t>(w * h * 4)) return false;
        std::memcpy(out.bgra.data(), mipData, w * h * 4);
    } else {
        return false;
    }

    return true;
}

} // namespace mapedit
