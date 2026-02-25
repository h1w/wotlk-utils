#pragma once
#include <string>

namespace mapedit {

class WorldGraphData;

class MainMenu {
public:
    struct Actions {
        bool openMmapDir = false;
        bool openGraph = false;
        bool openRoadGraph = false;
        bool saveGraph = false;
        bool saveGraphAs = false;
        bool saveRoadGraph = false;
        bool saveRoadGraphAs = false;
        bool quit = false;
        std::string graphFilePath;
        std::string roadGraphFilePath;
    };

    Actions Render(const WorldGraphData& graph, const WorldGraphData& roadGraph);
};

} // namespace mapedit
