#include "tile_index.h"
#include <filesystem>
#include <map>
#include <glog/logging.h>

namespace mapedit {

const std::set<TileCoord> TileIndex::s_empty;

void TileIndex::ScanDirectory(const std::string& dir) {
    m_dir = dir;
    m_tiles.clear();
    m_scanned = true;

    namespace fs = std::filesystem;
    if (!fs::is_directory(dir)) {
        LOG(ERROR) << "[TileIndex] Not a directory: " << dir;
        return;
    }

    int count = 0;
    for (const auto& entry : fs::directory_iterator(dir)) {
        if (!entry.is_regular_file()) continue;
        auto filename = entry.path().filename().string();
        // Format: {mapId:03d}{tileX:02d}{tileY:02d}.mmtile
        if (filename.size() < 11 || filename.substr(filename.size() - 7) != ".mmtile")
            continue;

        auto stem = filename.substr(0, filename.size() - 7); // remove .mmtile
        if (stem.size() != 7) continue; // 3+2+2 = 7 digits

        try {
            uint32_t mapId = std::stoul(stem.substr(0, 3));
            int tileX = std::stoi(stem.substr(3, 2));
            int tileY = std::stoi(stem.substr(5, 2));
            m_tiles[mapId].insert({tileX, tileY});
            ++count;
        } catch (...) {
            continue;
        }
    }

    LOG(INFO) << "[TileIndex] Scanned " << dir << ": " << count << " tiles across "
              << m_tiles.size() << " maps";
}

bool TileIndex::HasTile(uint32_t mapId, int tileX, int tileY) const {
    auto it = m_tiles.find(mapId);
    if (it == m_tiles.end()) return false;
    return it->second.count({tileX, tileY}) > 0;
}

const std::set<TileCoord>& TileIndex::GetTilesForMap(uint32_t mapId) const {
    auto it = m_tiles.find(mapId);
    if (it == m_tiles.end()) return s_empty;
    return it->second;
}

std::vector<uint32_t> TileIndex::GetMapIds() const {
    std::vector<uint32_t> ids;
    ids.reserve(m_tiles.size());
    for (auto& [id, _] : m_tiles)
        ids.push_back(id);
    return ids;
}

} // namespace mapedit
