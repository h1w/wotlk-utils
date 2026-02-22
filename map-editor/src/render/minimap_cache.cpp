#include "minimap_cache.h"
#include "../canvas/canvas.h"
#include "../mpq/mpq_archive.h"
#include "../mpq/dbc_reader.h"
#include "../mpq/blp_decoder.h"

#include <d3d11.h>
#include <imgui.h>
#include <glog/logging.h>

#include <algorithm>
#include <cstdio>
#include <sstream>

namespace mapedit {

static constexpr float kTileSize = 533.33333f;

// Parse md5translate.trs from MPQ.
// Format: tab-separated lines "dir\filename\thash\r\n"
// Example: "Azeroth\map32_48\t5a3b7c9d1e...\r\n"
// We build: key="azeroth\map32_48" -> path="Textures\Minimap\5a3b7c9d1e....blp"
static std::map<std::string, std::string> ParseMd5Translate(const std::vector<uint8_t>& data) {
    std::map<std::string, std::string> result;
    std::string content(reinterpret_cast<const char*>(data.data()), data.size());
    std::istringstream stream(content);
    std::string line;
    while (std::getline(stream, line)) {
        // Strip \r
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (line.empty()) continue;

        // Split on tab
        auto tabPos = line.find('\t');
        if (tabPos == std::string::npos) continue;

        std::string key = line.substr(0, tabPos);
        std::string hash = line.substr(tabPos + 1);

        // Trim whitespace from hash
        while (!hash.empty() && (hash.back() == ' ' || hash.back() == '\t'))
            hash.pop_back();
        if (hash.empty()) continue;

        // Normalize key to lowercase for case-insensitive lookup
        std::string lowerKey = key;
        std::transform(lowerKey.begin(), lowerKey.end(), lowerKey.begin(), ::tolower);

        // Strip .blp extension from key if present (some trs files include it)
        if (lowerKey.size() > 4 && lowerKey.substr(lowerKey.size() - 4) == ".blp")
            lowerKey.resize(lowerKey.size() - 4);

        // Strip .blp from hash if present to avoid double extension
        if (hash.size() > 4 && hash.substr(hash.size() - 4) == ".blp")
            hash.resize(hash.size() - 4);

        result[lowerKey] = "Textures\\Minimap\\" + hash + ".blp";
    }
    return result;
}

bool MinimapTileCache::Initialize(MpqArchiveSet& mpq) {
    // Shut down existing worker if re-initializing
    Shutdown();

    // Parse Map.dbc to build mapId -> InternalName lookup
    auto dbcData = mpq.ReadFile("DBFilesClient\\Map.dbc");
    if (dbcData.empty()) {
        LOG(ERROR) << "[MinimapCache] Failed to read Map.dbc from MPQ";
        return false;
    }

    DbcReader dbc;
    if (!dbc.Load(dbcData)) {
        LOG(ERROR) << "[MinimapCache] Failed to parse Map.dbc";
        return false;
    }

    m_mapNames.clear();
    for (uint32_t i = 0; i < dbc.GetRecordCount(); i++) {
        uint32_t mapId = dbc.GetUInt(i, 0);
        const char* name = dbc.GetString(i, 1);
        if (name && name[0])
            m_mapNames[mapId] = name;
    }

    LOG(INFO) << "[MinimapCache] Parsed Map.dbc: " << m_mapNames.size() << " maps";

    // Parse md5translate.trs — maps tile coordinates to BLP filenames
    auto trsData = mpq.ReadFile("Textures\\Minimap\\md5translate.trs");
    if (trsData.empty()) {
        LOG(ERROR) << "[MinimapCache] Failed to read Textures\\Minimap\\md5translate.trs";
        return false;
    }

    m_tilePathLookup = ParseMd5Translate(trsData);
    LOG(INFO) << "[MinimapCache] Parsed md5translate.trs: " << m_tilePathLookup.size() << " entries";

    // Log a few sample entries for diagnostics
    int logged = 0;
    for (auto& [key, path] : m_tilePathLookup) {
        if (key.find("azeroth") != std::string::npos && logged < 3) {
            LOG(INFO) << "[MinimapCache]   sample: '" << key << "' -> " << path;
            logged++;
        }
    }

    // Start worker thread
    m_running = true;
    m_worker = std::thread(&MinimapTileCache::WorkerLoop, this);

    return true;
}

void MinimapTileCache::Shutdown() {
    if (m_running) {
        m_running = false;
        m_requestCV.notify_all();
        if (m_worker.joinable())
            m_worker.join();
    }

    ClearCache();

    {
        std::lock_guard<std::mutex> lock(m_resultMutex);
        m_results.clear();
    }
    {
        std::lock_guard<std::mutex> lock(m_requestMutex);
        m_requests.clear();
    }
    m_pending.clear();
    m_missingTiles.clear();
    m_currentMapId = 0xFFFFFFFF;
    m_currentMapName.clear();
}

void MinimapTileCache::SetMap(uint32_t mapId) {
    if (mapId == m_currentMapId) return;

    m_currentMapId = mapId;

    auto it = m_mapNames.find(mapId);
    if (it != m_mapNames.end())
        m_currentMapName = it->second;
    else
        m_currentMapName.clear();

    ClearCache();

    {
        std::lock_guard<std::mutex> lock(m_requestMutex);
        m_requests.clear();
    }
    {
        std::lock_guard<std::mutex> lock(m_resultMutex);
        m_results.clear();
    }
    m_pending.clear();
    m_missingTiles.clear();
    m_allLoaded = false;

    if (!m_currentMapName.empty())
        LOG(INFO) << "[MinimapCache] Set map " << mapId << " (" << m_currentMapName << ")";
}

void MinimapTileCache::UpdateViewport(const Canvas& /*canvas*/, MpqArchiveSet& mpq,
                                       ID3D11Device* device) {
    if (m_currentMapName.empty()) return;

    // Load ALL tiles for the current map once (full 64x64 scan)
    if (!m_allLoaded) {
        m_allLoaded = true;
        int requested = 0;

        for (int tx = 0; tx <= 63; ++tx) {
            for (int ty = 0; ty <= 63; ++ty) {
                TileKey key = {tx, ty};

                if (m_cache.count(key)) continue;
                if (m_pending.count(key)) continue;
                if (m_missingTiles.count(key)) continue;

                // Build lookup key: "mapname\mapXX_YY" (lowercase)
                // WoW minimap convention: XX = from wowY (east-west), YY = from wowX (north-south)
                // Our tx = from wowX, ty = from wowY, so swap: map{ty}_{tx}
                char lookupKey[256];
                std::snprintf(lookupKey, sizeof(lookupKey), "%s\\map%02d_%02d",
                              m_currentMapName.c_str(), ty, tx);
                for (char* p = lookupKey; *p; ++p)
                    *p = static_cast<char>(::tolower(static_cast<unsigned char>(*p)));

                auto it = m_tilePathLookup.find(lookupKey);
                if (it == m_tilePathLookup.end()) {
                    m_missingTiles.insert(key);
                    continue;
                }

                // Read BLP from the resolved MPQ path (fast, <1ms per file)
                auto blpData = mpq.ReadFile(it->second);
                if (blpData.empty()) {
                    m_missingTiles.insert(key);
                    continue;
                }

                m_pending.insert(key);
                requested++;
                {
                    std::lock_guard<std::mutex> lock(m_requestMutex);
                    m_requests.push_back({tx, ty, std::move(blpData)});
                }
                m_requestCV.notify_one();
            }
        }

        if (requested > 0)
            LOG(INFO) << "[MinimapCache] Queued ALL " << requested << " tiles for decode";
    }

    ProcessCompletedTiles(device);
}

void MinimapTileCache::WorkerLoop() {
    while (m_running) {
        DecodeRequest req;
        {
            std::unique_lock<std::mutex> lock(m_requestMutex);
            m_requestCV.wait(lock, [this] {
                return !m_requests.empty() || !m_running;
            });
            if (!m_running) return;
            req = std::move(m_requests.front());
            m_requests.pop_front();
        }

        BlpImage img;
        if (DecodeBlp(req.blpData.data(), req.blpData.size(), img)) {
            std::lock_guard<std::mutex> lock(m_resultMutex);
            m_results.push_back({req.tileX, req.tileY,
                                 std::move(img.bgra), img.width, img.height});
        } else {
            std::lock_guard<std::mutex> lock(m_resultMutex);
            m_results.push_back({req.tileX, req.tileY, {}, 0, 0});
        }
    }
}

void MinimapTileCache::ProcessCompletedTiles(ID3D11Device* device) {
    std::lock_guard<std::mutex> lock(m_resultMutex);

    while (!m_results.empty()) {
        DecodeResult result = std::move(m_results.front());
        m_results.pop_front();

        TileKey key = {result.tileX, result.tileY};
        m_pending.erase(key);

        if (result.bgra.empty() || result.width <= 0 || result.height <= 0)
            continue;

        D3D11_TEXTURE2D_DESC desc = {};
        desc.Width = result.width;
        desc.Height = result.height;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

        D3D11_SUBRESOURCE_DATA initData = {};
        initData.pSysMem = result.bgra.data();
        initData.SysMemPitch = result.width * 4;

        ID3D11Texture2D* tex = nullptr;
        HRESULT hr = device->CreateTexture2D(&desc, &initData, &tex);
        if (FAILED(hr)) continue;

        D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
        srvDesc.Format = desc.Format;
        srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Texture2D.MipLevels = 1;

        ID3D11ShaderResourceView* srv = nullptr;
        hr = device->CreateShaderResourceView(tex, &srvDesc, &srv);
        tex->Release();

        if (FAILED(hr)) continue;

        m_cache[key] = {srv};
    }
}

void MinimapTileCache::Render(const Canvas& canvas) {
    if (m_cache.empty()) return;

    auto* dl = ImGui::GetBackgroundDrawList();
    ImU32 tint = IM_COL32(255, 255, 255, static_cast<int>(m_opacity * 255));

    for (auto& [key, tile] : m_cache) {
        if (!tile.srv) continue;

        int tx = key.first;
        int ty = key.second;

        // Minimap BLP tiles are offset by 1 tile from the ADT grid origin.
        // BLP "map{ty}_{tx}" covers the area one tile north+east of ADT tile (tx, ty).
        float worldMinX = (31 - tx) * kTileSize;
        float worldMaxX = (32 - tx) * kTileSize;
        float worldMinY = (31 - ty) * kTileSize;
        float worldMaxY = (32 - ty) * kTileSize;

        float sx1, sy1, sx2, sy2;
        canvas.WorldToScreen(worldMaxX, worldMaxY, sx1, sy1);
        canvas.WorldToScreen(worldMinX, worldMinY, sx2, sy2);

        // BLP row 0 = north, last row = south; screen top = north — no V-flip needed
        dl->AddImage(reinterpret_cast<ImTextureID>(tile.srv),
                     ImVec2(sx1, sy1), ImVec2(sx2, sy2),
                     ImVec2(0, 0), ImVec2(1, 1), tint);
    }
}

void MinimapTileCache::ForEachCachedTile(
    const std::function<void(int tx, int ty,
                             ID3D11ShaderResourceView* srv)>& callback) const {
    for (const auto& [key, tile] : m_cache) {
        if (tile.srv)
            callback(key.first, key.second, tile.srv);
    }
}

bool MinimapTileCache::HasMap(uint32_t mapId) const {
    return m_mapNames.count(mapId) > 0;
}

void MinimapTileCache::ClearCache() {
    for (auto& [key, tile] : m_cache) {
        if (tile.srv)
            tile.srv->Release();
    }
    m_cache.clear();
}

} // namespace mapedit
