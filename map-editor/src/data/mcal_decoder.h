#pragma once

#include <cstdint>
#include <cstddef>

namespace mapedit {

enum class AlphaFormat { Packed4Bit, Uncompressed8Bit, CompressedRle };

// Determine the alpha decode format for a layer.
// mclyFlags & 0x200 -> RLE compressed
// mphdFlags & 0x004 -> 8-bit uncompressed
// else              -> 4-bit packed
AlphaFormat GetAlphaFormat(uint32_t mphdFlags, uint32_t mclyFlags);

// Decode a single layer's alpha map from MCAL data into a 64x64 uint8 array.
// srcSize is the available bytes for this layer (needed for RLE boundary).
void DecodeAlpha(AlphaFormat fmt, const uint8_t* src, size_t srcSize, uint8_t out[64 * 64]);

} // namespace mapedit
