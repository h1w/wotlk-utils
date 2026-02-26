#include "bc1_compressor.h"

#define STB_DXT_IMPLEMENTATION
#include "../third_party/stb_dxt.h"

#include <algorithm>
#include <cstring>

namespace mapedit {

// Box-filter downsample RGBA image by 2x. Handles odd dimensions.
static std::vector<uint8_t> DownsampleRGBA(const uint8_t* src, uint32_t w, uint32_t h,
                                            uint32_t& outW, uint32_t& outH) {
    outW = (std::max)(1u, w / 2);
    outH = (std::max)(1u, h / 2);
    std::vector<uint8_t> dst(outW * outH * 4);

    for (uint32_t y = 0; y < outH; ++y) {
        for (uint32_t x = 0; x < outW; ++x) {
            uint32_t sx = x * 2;
            uint32_t sy = y * 2;
            uint32_t sx1 = (std::min)(sx + 1, w - 1);
            uint32_t sy1 = (std::min)(sy + 1, h - 1);

            const uint8_t* p00 = src + (sy  * w + sx)  * 4;
            const uint8_t* p10 = src + (sy  * w + sx1) * 4;
            const uint8_t* p01 = src + (sy1 * w + sx)  * 4;
            const uint8_t* p11 = src + (sy1 * w + sx1) * 4;

            uint8_t* out = dst.data() + (y * outW + x) * 4;
            for (int c = 0; c < 4; ++c) {
                out[c] = static_cast<uint8_t>(
                    (p00[c] + p10[c] + p01[c] + p11[c] + 2) / 4);
            }
        }
    }
    return dst;
}

CompressedAtlas CompressToBC1WithMips(const uint8_t* bgraData, uint32_t width, uint32_t height) {
    CompressedAtlas atlas;
    if (!bgraData || width == 0 || height == 0)
        return atlas;

    atlas.width  = width;
    atlas.height = height;

    // Count mip levels: floor(log2(max(w,h))) + 1
    uint32_t maxDim = (std::max)(width, height);
    atlas.mipCount = 1;
    while ((maxDim >> atlas.mipCount) > 0)
        ++atlas.mipCount;

    // Convert BGRA -> RGBA for mip level 0
    std::vector<uint8_t> rgbaData(width * height * 4);
    for (uint32_t i = 0; i < width * height; ++i) {
        rgbaData[i * 4 + 0] = bgraData[i * 4 + 2]; // R
        rgbaData[i * 4 + 1] = bgraData[i * 4 + 1]; // G
        rgbaData[i * 4 + 2] = bgraData[i * 4 + 0]; // B
        rgbaData[i * 4 + 3] = bgraData[i * 4 + 3]; // A
    }

    // Pre-calculate total BC1 size
    uint32_t totalSize = 0;
    {
        uint32_t mw = width, mh = height;
        for (uint32_t m = 0; m < atlas.mipCount; ++m) {
            uint32_t blocksX = (std::max)(1u, (mw + 3) / 4);
            uint32_t blocksY = (std::max)(1u, (mh + 3) / 4);
            totalSize += blocksX * blocksY * 8;
            mw = (std::max)(1u, mw / 2);
            mh = (std::max)(1u, mh / 2);
        }
    }

    atlas.bc1Data.resize(totalSize);
    atlas.mipOffsets.resize(atlas.mipCount);
    atlas.mipSizes.resize(atlas.mipCount);

    uint32_t offset = 0;
    uint32_t mw = width, mh = height;
    const uint8_t* currentRGBA = rgbaData.data();
    std::vector<uint8_t> mipBuffer; // holds downsampled data for mip > 0

    for (uint32_t mip = 0; mip < atlas.mipCount; ++mip) {
        uint32_t blocksX = (std::max)(1u, (mw + 3) / 4);
        uint32_t blocksY = (std::max)(1u, (mh + 3) / 4);
        uint32_t mipSize = blocksX * blocksY * 8;

        atlas.mipOffsets[mip] = offset;
        atlas.mipSizes[mip]   = mipSize;

        // Compress each 4x4 block
        uint8_t block[64]; // 4x4 RGBA = 64 bytes
        uint8_t* dst = atlas.bc1Data.data() + offset;

        for (uint32_t by = 0; by < blocksY; ++by) {
            for (uint32_t bx = 0; bx < blocksX; ++bx) {
                // Extract 4x4 block, clamping to image bounds
                for (int py = 0; py < 4; ++py) {
                    for (int px = 0; px < 4; ++px) {
                        uint32_t sx = (std::min)(bx * 4 + px, mw - 1);
                        uint32_t sy = (std::min)(by * 4 + py, mh - 1);
                        const uint8_t* src = currentRGBA + (sy * mw + sx) * 4;
                        std::memcpy(block + (py * 4 + px) * 4, src, 4);
                    }
                }
                stb_compress_dxt_block(dst, block, 0, STB_DXT_NORMAL);
                dst += 8;
            }
        }

        offset += mipSize;

        // Downsample for next mip
        if (mip + 1 < atlas.mipCount) {
            uint32_t newW, newH;
            mipBuffer = DownsampleRGBA(currentRGBA, mw, mh, newW, newH);
            mw = newW;
            mh = newH;
            currentRGBA = mipBuffer.data();
        }
    }

    return atlas;
}

} // namespace mapedit
