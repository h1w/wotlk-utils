#include "route_data.h"

#include <nlohmann/json.hpp>
#include <glog/logging.h>
#include <fstream>
#include <cstdio>

using json = nlohmann::json;

namespace mapedit {

// Parse "#RRGGBB" to ABGR packed uint32
static uint32_t ParseHexColor(const std::string& s) {
    if (s.size() >= 7 && s[0] == '#') {
        unsigned int r, g, b;
        if (sscanf(s.c_str() + 1, "%02x%02x%02x", &r, &g, &b) == 3)
            return 0xFF000000 | (b << 16) | (g << 8) | r; // ABGR
    }
    return 0xFF00FFFF; // default cyan
}

static std::string ToHexColor(uint32_t abgr) {
    char buf[8];
    uint8_t r = abgr & 0xFF;
    uint8_t g = (abgr >> 8) & 0xFF;
    uint8_t b = (abgr >> 16) & 0xFF;
    snprintf(buf, sizeof(buf), "#%02X%02X%02X", r, g, b);
    return buf;
}

bool RouteData::LoadFromFile(const std::string& path) {
    m_routes.clear();
    m_dirty = false;
    m_loaded = false;

    std::ifstream file(path);
    if (!file.is_open()) {
        LOG(ERROR) << "[RouteData] Cannot open: " << path;
        return false;
    }

    json doc;
    try {
        doc = json::parse(file);
    } catch (const json::parse_error& e) {
        LOG(ERROR) << "[RouteData] Parse error: " << e.what();
        return false;
    }

    if (doc.contains("routes") && doc["routes"].is_array()) {
        for (const auto& jr : doc["routes"]) {
            Route route;
            route.name  = jr.value("name", "Unnamed");
            route.mapId = jr.value("map", 0u);
            route.color = ParseHexColor(jr.value("color", "#00FFFF"));
            route.loop  = jr.value("loop", false);

            if (jr.contains("waypoints") && jr["waypoints"].is_array()) {
                for (const auto& jw : jr["waypoints"]) {
                    RouteWaypoint wp;
                    wp.x = jw.value("x", 0.0f);
                    wp.y = jw.value("y", 0.0f);
                    wp.z = jw.value("z", 0.0f);
                    route.waypoints.push_back(wp);
                }
            }
            m_routes.push_back(std::move(route));
        }
    }

    m_filePath = path;
    m_loaded = true;
    LOG(INFO) << "[RouteData] Loaded " << m_routes.size() << " routes from " << path;
    return true;
}

bool RouteData::SaveToFile(const std::string& path) const {
    json doc;
    json routesArr = json::array();

    for (const auto& route : m_routes) {
        json jr;
        jr["name"]  = route.name;
        jr["map"]   = route.mapId;
        jr["color"] = ToHexColor(route.color);
        jr["loop"]  = route.loop;

        json wps = json::array();
        for (const auto& wp : route.waypoints) {
            json jw;
            jw["x"] = wp.x;
            jw["y"] = wp.y;
            jw["z"] = wp.z;
            wps.push_back(jw);
        }
        jr["waypoints"] = wps;
        routesArr.push_back(jr);
    }

    doc["routes"] = routesArr;

    std::ofstream file(path);
    if (!file.is_open()) {
        LOG(ERROR) << "[RouteData] Cannot write: " << path;
        return false;
    }

    file << doc.dump(2);
    LOG(INFO) << "[RouteData] Saved " << m_routes.size() << " routes to " << path;
    return true;
}

void RouteData::AddRoute(const Route& route) {
    m_routes.push_back(route);
    m_dirty = true;
}

void RouteData::RemoveRoute(size_t index) {
    if (index < m_routes.size()) {
        m_routes.erase(m_routes.begin() + index);
        m_dirty = true;
    }
}

void RouteData::SetState(std::vector<Route> routes) {
    m_routes = std::move(routes);
    m_dirty = true;
}

} // namespace mapedit
