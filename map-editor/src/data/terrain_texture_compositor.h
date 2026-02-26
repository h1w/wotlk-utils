#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "../mpq/blp_decoder.h"

namespace mapedit {

class MpqArchiveSet;
struct AdtTextureData;
struct AdtChunkTexture;

// Thread-safe BLP texture cache. Shared across tiles to avoid
// reloading the same ground textures (tileable, ~20 unique per tile).
class BlpTextureCache {
public:
    // Returns pointer to cached image, or nullptr if load failed.
    // Thread-safe: locks internally.
    const BlpImage* Get(const std::string& path, MpqArchiveSet& mpq);

private:
    std::mutex m_mutex;
    std::unordered_map<std::string, BlpImage> m_cache;
    // Sentinel for failed loads (empty image, width==0).
    static constexpr int kFailedSentinel = 0;
};

class TerrainTextureCompositor {
public:
    void SetBlpCache(BlpTextureCache* cache) { m_blpCache = cache; }

    // Composite all 256 chunks into a 1024x1024 BGRA atlas.
    // Returns empty vector on failure.
    std::vector<uint8_t> CompositeTileAtlas(const AdtTextureData& adt,
                                             uint32_t mphdFlags,
                                             MpqArchiveSet& mpq);

private:
    BlpTextureCache* m_blpCache = nullptr;

    // Composite a single chunk (64x64 BGRA output).
    void CompositeChunk(const AdtChunkTexture& chunk,
                        const std::vector<std::string>& texPaths,
                        uint32_t mphdFlags,
                        MpqArchiveSet& mpq,
                        uint8_t out[64][64][4]);
};

} // namespace mapedit
