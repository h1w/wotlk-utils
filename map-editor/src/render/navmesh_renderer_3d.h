#pragma once

#include <d3d11.h>
#include <cstdint>
#include <map>
#include <vector>
#include <utility>

#include "navmesh_pipeline_3d.h"

namespace mapedit {

struct Camera3D;
class TileCache;

class NavmeshRenderer3D {
public:
    bool Initialize(ID3D11Device* device, ID3D11DeviceContext* context);
    void Shutdown();

    // Render all visible navmesh tiles in 3D.
    // Called directly (NOT via ImGui callback) before ImGui::Render().
    void Render(const Camera3D& camera, TileCache& cache,
                ID3D11RenderTargetView* rtv, ID3D11DepthStencilView* dsv,
                NavmeshColorMode colorMode = NavmeshColorMode::FlatGreen,
                bool drawEdges = true,
                float heightMin = 0.0f, float heightMax = 500.0f);

private:
    using TileKey = std::pair<int, int>;

    struct TileGpuData3D {
        ID3D11Buffer* detailVB = nullptr;
        UINT detailVertCount = 0;
        ID3D11Buffer* baseVB = nullptr;
        UINT baseVertCount = 0;
        // 3D AABB for frustum culling
        float minX = 0, minY = 0, minZ = 0;
        float maxX = 0, maxY = 0, maxZ = 0;
    };

    void SyncGpuCache(TileCache& cache);
    void ReleaseGpuTile(TileGpuData3D& tile);
    ID3D11Buffer* CreateVB(const std::vector<NavmeshVertex3D>& verts);

    static bool FrustumIntersectsAABB(const float planes[6][4],
                                       const TileGpuData3D& tile);

public:
    // Per-frame stats (updated during Render)
    mutable int statDrawCalls = 0;
    mutable int statVertices  = 0;

private:
    ID3D11Device*        m_device  = nullptr;
    ID3D11DeviceContext* m_context = nullptr;
    NavmeshPipeline3D    m_pipeline;
    std::map<TileKey, TileGpuData3D> m_gpuCache;
    uint32_t             m_lastSyncMapId = 0xFFFFFFFF;
};

} // namespace mapedit
