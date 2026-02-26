#pragma once

#include <cstdint>
#include <vector>

namespace mapedit {

// BC1 (DXT1) compressed texture atlas with full mip chain.
struct CompressedAtlas {
    std::vector<uint8_t>  bc1Data;     // all mip levels concatenated
    std::vector<uint32_t> mipOffsets;  // byte offset per mip level
    std::vector<uint32_t> mipSizes;   // byte size per mip level
    uint32_t width = 0, height = 0, mipCount = 0;
};

// Compress a BGRA atlas to BC1 with full mip chain (box-filter downsample).
// Input: bgraData is width*height*4 bytes (B8G8R8A8).
// Returns empty atlas on failure.
CompressedAtlas CompressToBC1WithMips(const uint8_t* bgraData, uint32_t width, uint32_t height);

} // namespace mapedit
