#include "terrain_texture_compositor.h"
#include "adt_texture_parser.h"
#include "mcal_decoder.h"
#include "../mpq/mpq_archive.h"

#include <glog/logging.h>

#include <algorithm>
#include <cstring>

namespace mapedit {

// ---------------------------------------------------------------------------
// BlpTextureCache
// ---------------------------------------------------------------------------

const BlpImage* BlpTextureCache::Get(const std::string& path, MpqArchiveSet& mpq) {
    std::lock_guard<std::mutex> lock(m_mutex);

    auto it = m_cache.find(path);
    if (it != m_cache.end()) {
        if (it->second.width == 0)
            return nullptr;  // Failed sentinel
        return &it->second;
    }

    // Load from MPQ
    auto data = mpq.ReadFile(path);
    if (data.empty()) {
        m_cache[path] = BlpImage{};  // sentinel
        return nullptr;
    }

    BlpImage img;
    if (!DecodeBlp(data.data(), data.size(), img) || img.width == 0) {
        m_cache[path] = BlpImage{};  // sentinel
        return nullptr;
    }

    auto [ins, _] = m_cache.emplace(path, std::move(img));
    return &ins->second;
}

// ---------------------------------------------------------------------------
// Compositor helpers
// ---------------------------------------------------------------------------

// Sample a BLP texture at (u, v) in [0, 64) with wrapping.
// BLP textures tile, so we wrap at their dimensions.
static void SampleTexture(const BlpImage& img, int px, int py, uint8_t out[4]) {
    // Map 64x64 chunk pixel to texture UV.
    // Chunk is 33.33 yards, texture tiles at some period.
    // WoW ground textures are typically 64x64 or 128x128 or 256x256.
    // We just sample with wrapping.
    int tx = px % img.width;
    int ty = py % img.height;
    if (tx < 0) tx += img.width;
    if (ty < 0) ty += img.height;

    size_t idx = (static_cast<size_t>(ty) * img.width + tx) * 4;
    out[0] = img.bgra[idx + 0];  // B
    out[1] = img.bgra[idx + 1];  // G
    out[2] = img.bgra[idx + 2];  // R
    out[3] = img.bgra[idx + 3];  // A
}

void TerrainTextureCompositor::CompositeChunk(const AdtChunkTexture& chunk,
                                               const std::vector<std::string>& texPaths,
                                               uint32_t mphdFlags,
                                               MpqArchiveSet& mpq,
                                               uint8_t out[64][64][4]) {
    // Start with black
    std::memset(out, 0, 64 * 64 * 4);

    if (chunk.nLayers == 0 || !m_blpCache)
        return;

    // Layer 0: base texture at full opacity
    if (chunk.layers[0].textureId < texPaths.size()) {
        const BlpImage* base = m_blpCache->Get(texPaths[chunk.layers[0].textureId], mpq);
        if (base) {
            for (int y = 0; y < 64; ++y) {
                for (int x = 0; x < 64; ++x) {
                    SampleTexture(*base, x, y, out[y][x]);
                }
            }
        }
    }

    // Layers 1-3: alpha-blended on top
    for (int li = 1; li < chunk.nLayers && li < 4; ++li) {
        const auto& layer = chunk.layers[li];
        if (layer.textureId >= texPaths.size())
            continue;

        const BlpImage* tex = m_blpCache->Get(texPaths[layer.textureId], mpq);
        if (!tex) continue;

        // Decode alpha map
        uint8_t alpha[64 * 64];
        if (!layer.alphaRaw.empty() && (layer.flags & 0x100)) {
            AlphaFormat fmt = GetAlphaFormat(mphdFlags, layer.flags);
            DecodeAlpha(fmt, layer.alphaRaw.data(), layer.alphaRaw.size(), alpha);
        } else {
            // No alpha: full opacity (shouldn't happen for layer > 0 with flag 0x100)
            std::memset(alpha, 255, sizeof(alpha));
        }

        // Alpha-blend this layer on top
        for (int y = 0; y < 64; ++y) {
            for (int x = 0; x < 64; ++x) {
                uint8_t a = alpha[y * 64 + x];
                if (a == 0) continue;

                uint8_t texel[4];
                SampleTexture(*tex, x, y, texel);

                if (a == 255) {
                    out[y][x][0] = texel[0];
                    out[y][x][1] = texel[1];
                    out[y][x][2] = texel[2];
                    out[y][x][3] = 255;
                } else {
                    // Blend: dst = dst*(1-a) + src*a
                    uint8_t ia = 255 - a;
                    out[y][x][0] = static_cast<uint8_t>((out[y][x][0] * ia + texel[0] * a) / 255);
                    out[y][x][1] = static_cast<uint8_t>((out[y][x][1] * ia + texel[1] * a) / 255);
                    out[y][x][2] = static_cast<uint8_t>((out[y][x][2] * ia + texel[2] * a) / 255);
                    out[y][x][3] = 255;
                }
            }
        }
    }

    // Ensure full alpha for all pixels
    for (int y = 0; y < 64; ++y)
        for (int x = 0; x < 64; ++x)
            out[y][x][3] = 255;
}

// ---------------------------------------------------------------------------
// Atlas compositing
// ---------------------------------------------------------------------------

std::vector<uint8_t> TerrainTextureCompositor::CompositeTileAtlas(
    const AdtTextureData& adt, uint32_t mphdFlags, MpqArchiveSet& mpq) {

    if (!adt.valid || adt.texturePaths.empty())
        return {};

    // 1024x1024 BGRA atlas
    std::vector<uint8_t> atlas(1024 * 1024 * 4, 0);

    uint8_t chunkBuf[64][64][4];

    for (int row = 0; row < 16; ++row) {
        for (int col = 0; col < 16; ++col) {
            const auto& chunk = adt.chunks[row][col];
            CompositeChunk(chunk, adt.texturePaths, mphdFlags, mpq, chunkBuf);

            // Copy chunk into atlas at (col*64, row*64)
            int baseX = col * 64;
            int baseY = row * 64;
            for (int y = 0; y < 64; ++y) {
                size_t dstOff = (static_cast<size_t>(baseY + y) * 1024 + baseX) * 4;
                std::memcpy(atlas.data() + dstOff, chunkBuf[y], 64 * 4);
            }
        }
    }

    return atlas;
}

} // namespace mapedit
