#pragma once
#include <cstdint>

namespace mapedit {

struct Canvas;
struct Camera3D;
class TerrainHeightSampler;
class IssuePanel;

class StatusBar {
public:
    void Render(const Canvas& canvas, uint32_t mapId, const char* mapName,
                TerrainHeightSampler* heightSampler = nullptr,
                IssuePanel* issuePanel = nullptr);
    void Render3D(const Camera3D& camera, uint32_t mapId, const char* mapName,
                  TerrainHeightSampler* heightSampler = nullptr,
                  IssuePanel* issuePanel = nullptr);
};

} // namespace mapedit
