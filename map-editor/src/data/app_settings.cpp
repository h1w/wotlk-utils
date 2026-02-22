#include "app_settings.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <nlohmann/json.hpp>
#include <glog/logging.h>
#include <filesystem>
#include <fstream>

using json = nlohmann::json;
namespace fs = std::filesystem;

namespace mapedit {

std::string AppSettings::GetSettingsPath() {
    char buf[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, buf, MAX_PATH);
    fs::path exeDir = fs::path(buf).parent_path();
    return (exeDir / "map_editor_settings.json").string();
}

bool AppSettings::Load() {
    const std::string path = GetSettingsPath();

    std::ifstream file(path);
    if (!file.is_open()) {
        LOG(INFO) << "[AppSettings] No settings file found at " << path
                  << " — using defaults";
        return false;
    }

    json doc;
    try {
        doc = json::parse(file);
    } catch (const json::parse_error& e) {
        LOG(ERROR) << "[AppSettings] Parse error: " << e.what();
        return false;
    }

    mmapDir        = doc.value("mmap_dir", std::string());
    wowDir         = doc.value("wow_dir", std::string());
    worldGraphPath = doc.value("world_graph_path", std::string());
    routesPath     = doc.value("routes_path", std::string());
    lastMapId      = doc.value("last_map_id", 0u);
    bgOpacity      = doc.value("bg_opacity", 0.3f);
    autoBgFromMpq  = doc.value("auto_bg_from_mpq", true);
    showLegend     = doc.value("show_legend", true);
    bgMode         = doc.value("bg_mode", 1);

    // Layer visibility
    if (doc.contains("layers") && doc["layers"].is_object()) {
        const auto& layers = doc["layers"];
        showGrid    = layers.value("grid", true);
        showNavmesh = layers.value("navmesh", true);
        showNodes   = layers.value("nodes", true);
        showEdges   = layers.value("edges", true);
        showLabels  = layers.value("labels", true);
        showRoutes  = layers.value("routes", true);
        showPath    = layers.value("path", true);
        showBackground = layers.value("background", true);
        navmeshMinZoom = layers.value("navmesh_min_zoom", 0.15f);
        navmeshMaxTiles = layers.value("navmesh_max_tiles", 200);
        showTerrain = layers.value("terrain", true);
        terrainColorMode = layers.value("terrain_color_mode", 0);
        showBuildings = layers.value("buildings", true);
    }

    // Window dimensions
    if (doc.contains("window") && doc["window"].is_object()) {
        const auto& win = doc["window"];
        windowWidth  = win.value("width", 0);
        windowHeight = win.value("height", 0);
    }

    LOG(INFO) << "[AppSettings] Loaded settings from " << path;
    return true;
}

bool AppSettings::Save() const {
    const std::string path = GetSettingsPath();

    json doc;

    doc["mmap_dir"]         = mmapDir;
    doc["wow_dir"]          = wowDir;
    doc["world_graph_path"] = worldGraphPath;
    doc["routes_path"]      = routesPath;
    doc["last_map_id"]      = lastMapId;
    doc["bg_opacity"]       = bgOpacity;
    doc["auto_bg_from_mpq"] = autoBgFromMpq;
    doc["show_legend"]      = showLegend;
    doc["bg_mode"]          = bgMode;

    doc["layers"] = {
        {"grid",    showGrid},
        {"navmesh", showNavmesh},
        {"nodes",   showNodes},
        {"edges",   showEdges},
        {"labels",  showLabels},
        {"routes",  showRoutes},
        {"path",    showPath},
        {"background", showBackground},
        {"navmesh_min_zoom", navmeshMinZoom},
        {"navmesh_max_tiles", navmeshMaxTiles},
        {"terrain", showTerrain},
        {"terrain_color_mode", terrainColorMode},
        {"buildings", showBuildings},
    };

    doc["window"] = {
        {"width",  windowWidth},
        {"height", windowHeight},
    };

    std::ofstream file(path);
    if (!file.is_open()) {
        LOG(ERROR) << "[AppSettings] Cannot write: " << path;
        return false;
    }

    file << doc.dump(2);
    LOG(INFO) << "[AppSettings] Saved settings to " << path;
    return true;
}

} // namespace mapedit
