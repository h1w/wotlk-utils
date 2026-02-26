#pragma once

#include <d3d11.h>
#include <cstdint>
#include <vector>
#include "primitives_3d.h" // NodeInstance

namespace mapedit {

struct Camera3D;
struct LayerVisibility;
struct MultiSelection;
class WorldGraphData;

// Caches 3D graph geometry (edge lines in a static VB, node instances in a vector).
// Rebuilds only when the graph version, selection, mapId, or visibility changes.
class GraphMeshCache {
public:
    void Initialize(ID3D11Device* device);
    void Shutdown();

    // Check if cache needs rebuild; if so, rebuild.
    // Returns true if cache has valid geometry to draw.
    bool Update(const WorldGraphData& graph, uint32_t mapId,
                const MultiSelection& selection, const LayerVisibility& layers,
                float dimAlpha);

    // Edge line geometry
    ID3D11Buffer* GetVB()          const { return m_vb; }
    uint32_t      GetVertexCount() const { return m_vertexCount; }
    bool          HasEdges()       const { return m_vb != nullptr && m_vertexCount > 0; }
    bool          IsValid()        const { return HasEdges() || !m_nodeInstances.empty(); }

    // Node instances (for instanced circle drawing)
    const std::vector<Primitives3D::NodeInstance>& GetNodeInstances() const { return m_nodeInstances; }

private:
    struct LineVertex {
        float    x, y, z;
        uint32_t color;
    };

    void Rebuild(const WorldGraphData& graph, uint32_t mapId,
                 const MultiSelection& selection, const LayerVisibility& layers,
                 float dimAlpha);

    ID3D11Device* m_device = nullptr;
    ID3D11Buffer* m_vb = nullptr;
    uint32_t m_vertexCount = 0;

    // Cached node instances for instanced drawing
    std::vector<Primitives3D::NodeInstance> m_nodeInstances;

    // Dirty tracking
    uint32_t m_graphVersion = 0;
    size_t   m_selectionHash = 0;
    uint32_t m_mapId = UINT32_MAX;
    bool     m_showEdges = false;
    bool     m_showNodes = false;
    float    m_dimAlpha = 1.0f;
};

} // namespace mapedit
