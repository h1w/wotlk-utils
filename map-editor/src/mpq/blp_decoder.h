#pragma once
#include <cstdint>
#include <vector>

namespace mapedit {

struct BlpImage {
    int width  = 0;
    int height = 0;
    std::vector<uint8_t> bgra; // width * height * 4 bytes, BGRA order
};

// Decode a BLP2 file (WoW 3.3.5a texture format) into BGRA pixels.
// Supports: paletted, DXT1, DXT3, DXT5, uncompressed BGRA.
// Returns false on unsupported format or corrupt data.
bool DecodeBlp(const uint8_t* data, size_t size, BlpImage& out);

} // namespace mapedit
