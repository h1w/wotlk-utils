#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace mapedit {

struct RouteWaypoint {
    float x = 0, y = 0, z = 0;
};

struct Route {
    std::string name;
    uint32_t mapId = 0;
    uint32_t color = 0xFF00FFFF; // ABGR packed (default: cyan)
    bool loop = false;
    std::vector<RouteWaypoint> waypoints;
};

class RouteData {
public:
    bool LoadFromFile(const std::string& path);
    bool SaveToFile(const std::string& path) const;

    std::vector<Route>& GetRoutes() { return m_routes; }
    const std::vector<Route>& GetRoutes() const { return m_routes; }

    void AddRoute(const Route& route);
    void RemoveRoute(size_t index);

    // Replace all route data (used by undo/redo).
    void SetState(std::vector<Route> routes);

    bool IsDirty() const { return m_dirty; }
    void ClearDirty() { m_dirty = false; }
    void MarkDirty() { m_dirty = true; }

    bool IsLoaded() const { return m_loaded; }
    const std::string& GetFilePath() const { return m_filePath; }

private:
    std::vector<Route> m_routes;
    bool m_dirty = false;
    bool m_loaded = false;
    std::string m_filePath;
};

} // namespace mapedit
