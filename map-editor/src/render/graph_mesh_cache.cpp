#include "graph_mesh_cache.h"
#include "graph_renderer.h"       // LayerVisibility
#include "../data/world_graph_data.h"
#include "../editor/selection.h"

#include <glog/logging.h>
#include <imgui.h>
#include <cmath>
#include <cstring>

namespace mapedit {

// ---------------------------------------------------------------------------
// Color helpers (mirror Graph3DRenderer exactly)
// ---------------------------------------------------------------------------

static uint32_t NodeColorABGR(NodeType type) {
    switch (type) {
    case NodeType::FlightMaster:    return IM_COL32(255, 200,   0, 255);
    case NodeType::Portal:          return IM_COL32(128,   0, 255, 255);
    case NodeType::BoatZeppelin:    return IM_COL32(  0, 150, 255, 255);
    case NodeType::InnKeeper:       return IM_COL32(  0, 200, 100, 255);
    case NodeType::ZoneBoundary:    return IM_COL32(200, 200, 200, 255);
    case NodeType::DungeonEntrance: return IM_COL32(200,  50,  50, 255);
    default:                        return IM_COL32(180, 180, 180, 255);
    }
}

static uint32_t EdgeColorABGR(EdgeType type) {
    switch (type) {
    case EdgeType::Flight:   return IM_COL32(255, 200,   0, 150);
    case EdgeType::Teleport: return IM_COL32(128,   0, 255, 150);
    case EdgeType::Boat:     return IM_COL32(  0, 150, 255, 150);
    default:                 return IM_COL32(150, 150, 150, 100);
    }
}

static uint32_t ApplyDim(uint32_t color, float dimAlpha) {
    if (dimAlpha >= 1.0f) return color;
    uint32_t a = (color >> 24) & 0xFF;
    a = static_cast<uint32_t>(a * dimAlpha);
    return (color & 0x00FFFFFF) | (a << 24);
}

// ---------------------------------------------------------------------------
// Initialize / Shutdown
// ---------------------------------------------------------------------------

void GraphMeshCache::Initialize(ID3D11Device* device) {
    m_device = device;
}

void GraphMeshCache::Shutdown() {
    if (m_vb) { m_vb->Release(); m_vb = nullptr; }
    m_vertexCount = 0;
    m_device = nullptr;
}

// ---------------------------------------------------------------------------
// Update — check dirty flags, rebuild if needed
// ---------------------------------------------------------------------------

bool GraphMeshCache::Update(const WorldGraphData& graph, uint32_t mapId,
                             const MultiSelection& selection,
                             const LayerVisibility& layers, float dimAlpha) {
    uint32_t ver = graph.GetVersion();
    size_t selHash = selection.GetHash();

    bool dirty = (ver != m_graphVersion) ||
                 (selHash != m_selectionHash) ||
                 (mapId != m_mapId) ||
                 (layers.showEdges != m_showEdges) ||
                 (layers.showNodes != m_showNodes) ||
                 (dimAlpha != m_dimAlpha);

    if (dirty) {
        Rebuild(graph, mapId, selection, layers, dimAlpha);
        m_graphVersion  = ver;
        m_selectionHash = selHash;
        m_mapId         = mapId;
        m_showEdges     = layers.showEdges;
        m_showNodes     = layers.showNodes;
        m_dimAlpha      = dimAlpha;
    }

    return IsValid();
}

// ---------------------------------------------------------------------------
// Rebuild — generate full geometry into a static VB
// ---------------------------------------------------------------------------

void GraphMeshCache::Rebuild(const WorldGraphData& graph, uint32_t mapId,
                              const MultiSelection& selection,
                              const LayerVisibility& layers, float dimAlpha) {
    std::vector<LineVertex> verts;
    verts.reserve(graph.GetEdges().size() * 2 + graph.GetNodes().size() * 32);

    // Edges
    if (layers.showEdges) {
        for (size_t i = 0; i < graph.GetEdges().size(); ++i) {
            const auto& edge = graph.GetEdges()[i];
            const auto* from = graph.GetNode(edge.fromNode);
            const auto* to   = graph.GetNode(edge.toNode);
            if (!from || !to) continue;
            if (from->mapId != mapId && to->mapId != mapId) continue;

            uint32_t color = ApplyDim(EdgeColorABGR(edge.type), dimAlpha);
            if (selection.IsEdgeSelected(i))
                color = IM_COL32(255, 255, 0, 255);

            verts.push_back({ from->x, from->y, from->z + 0.5f, color });
            verts.push_back({ to->x,   to->y,   to->z   + 0.5f, color });
        }
    }

    // Nodes (instanced circles)
    m_nodeInstances.clear();
    if (layers.showNodes) {
        for (const auto& node : graph.GetNodes()) {
            if (node.mapId != mapId) continue;

            uint32_t color = ApplyDim(NodeColorABGR(node.type), dimAlpha);
            float radius = 2.1f;
            bool selected = selection.IsNodeSelected(node.id);

            if (selected) {
                // White outer ring (larger radius)
                m_nodeInstances.push_back({ node.x, node.y, node.z + 0.5f,
                                            radius + 2.0f,
                                            IM_COL32(255, 255, 255, 220) });
            }

            m_nodeInstances.push_back({ node.x, node.y, node.z + 0.5f, radius, color });
        }
    }

    // Release old VB
    if (m_vb) { m_vb->Release(); m_vb = nullptr; }
    m_vertexCount = 0;

    if (verts.empty() || !m_device)
        return;

    D3D11_BUFFER_DESC desc = {};
    desc.ByteWidth = static_cast<UINT>(verts.size() * sizeof(LineVertex));
    desc.Usage     = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_VERTEX_BUFFER;

    D3D11_SUBRESOURCE_DATA init = {};
    init.pSysMem = verts.data();

    HRESULT hr = m_device->CreateBuffer(&desc, &init, &m_vb);
    if (FAILED(hr)) {
        LOG(WARNING) << "[GraphMeshCache] CreateBuffer failed: 0x" << std::hex << hr;
        return;
    }

    m_vertexCount = static_cast<uint32_t>(verts.size());
}

} // namespace mapedit
