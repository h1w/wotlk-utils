#pragma once

#include <d3d11.h>
#include <cstdint>
#include <map>
#include <vector>
#include <utility>

#include "navmesh_pipeline.h"

struct ImDrawList;
struct ImDrawCmd;

namespace mapedit {

struct Canvas;
class TileCache;

class NavmeshRenderer {
public:
    bool Initialize(ID3D11Device* device, ID3D11DeviceContext* context);
    void Shutdown();
    void Render(const Canvas& canvas, TileCache& cache, float minZoom);

private:
    using TileKey = std::pair<int, int>;

    struct TileGpuData {
        ID3D11Buffer* detailVB = nullptr;
        UINT detailVertCount = 0;
        ID3D11Buffer* baseVB = nullptr;
        UINT baseVertCount = 0;
        float minX = 0, minY = 0, maxX = 0, maxY = 0; // world AABB
    };

    struct VisibleTile {
        ID3D11Buffer* vb;
        UINT vertCount;
    };

    // Persistent callback data (lives for the frame)
    struct CallbackData {
        NavmeshRenderer* self;
        ID3D11DeviceContext* context;
        float vpX, vpY, vpW, vpH;
        float centerX, centerY, zoom;
        bool drawEdges;
        std::vector<VisibleTile> tiles;
    };

    void SyncGpuCache(TileCache& cache);
    void ReleaseGpuTile(TileGpuData& tile);
    ID3D11Buffer* CreateVB(const std::vector<NavmeshVertex>& verts);

    static void DrawCallback(const ImDrawList* parent_list, const ImDrawCmd* cmd);

    ID3D11Device*        m_device = nullptr;
    ID3D11DeviceContext* m_context = nullptr;
    NavmeshPipeline      m_pipeline;
    std::map<TileKey, TileGpuData> m_gpuCache;
    uint32_t             m_lastSyncMapId = 0xFFFFFFFF;
    CallbackData         m_cbData;
};

} // namespace mapedit
