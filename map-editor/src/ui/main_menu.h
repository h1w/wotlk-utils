#pragma once
#include <string>

namespace mapedit {

class WorldGraphData;

class MainMenu {
public:
    struct Actions {
        bool openMmapDir = false;
        bool openGraph = false;
        bool saveGraph = false;
        bool saveGraphAs = false;
        bool quit = false;
        std::string graphFilePath;
    };

    Actions Render(const WorldGraphData& graph);
};

} // namespace mapedit
