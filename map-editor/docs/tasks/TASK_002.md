# TASK-002: 3D Navmesh Viewing Mode

**Status**: DONE
**Created**: 2026-02-22
**Last Updated**: 2026-02-22

---

## Overview

Add a **3D rendering mode** to the map editor as an alternative to the existing 2D top-down view. The 3D mode renders navmesh geometry with actual elevation (Z coordinates), provides a third-person orbiting camera, and supports player movement emulation along pathfinding routes.

**Key constraint**: the existing 2D mode must remain completely untouched. The two modes are toggled via a UI switch. No textures, no game file terrain (ADT/WMO/M2) — only navmesh triangles in 3D.

**Location**: `map-editor/` in the `wotlk-utils` repository.

---

## Phase Status Summary

| Phase | Description | Status |
|-------|-------------|--------|
| Phase 1 | ViewMode abstraction + 3D camera system | NOT STARTED |
| Phase 2 | 3D navmesh GPU pipeline + renderer | NOT STARTED |
| Phase 3 | 3D overlays (graph, routes, path, grid) | NOT STARTED |
| Phase 4 | Player emulation (movement along paths) | NOT STARTED |
| Phase 5 | Polish (lighting, height coloring, LOD) | NOT STARTED |

---

## Architecture Decision: Two Isolated Modes

The 2D and 3D modes are **completely separate rendering paths** sharing the same data layer:

```
               App
              /   \
        [2D Mode]  [3D Mode]      ← toggle via UI
           |            |
        Canvas       Camera3D     ← different camera systems
           |            |
    NavmeshRenderer  Navmesh3DRenderer  ← different GPU pipelines
    GraphRenderer    Graph3DRenderer
    RouteRenderer    Route3DRenderer
    PathRenderer     Path3DRenderer
    GridRenderer     Grid3DRenderer
           \            /
            \          /
         Shared data layer:
         TileCache, TileIndex, WorldGraphData,
         RouteData, Pathfinder, Selection
```

**Why two separate modes, not unified 3D with ortho projection:**
- Zero risk to existing 2D code (no regression)
- 2D uses ImDrawList for most layers; 3D needs full DX11 for everything
- Camera, input, and coordinate transforms are fundamentally different
- Can be developed incrementally without breaking existing functionality

---

## Current State: What We Already Have

### Data available (Z coordinates already extracted)

`tile_loader.h` — `TileTriangle` stores `z[3]` (WoW Z for all 3 vertices):
```cpp
struct TileTriangle {
    float x[3]; // WoW X coords of 3 vertices
    float y[3]; // WoW Y coords
    float z[3]; // WoW Z coords  ← ALREADY EXTRACTED, currently DROPPED on GPU upload
};
```

### What gets dropped

`navmesh_renderer.cpp:BuildVertexData()` line 18-26 — Z is discarded:
```cpp
// CURRENT: only posX, posY uploaded
out.push_back({tri.x[0], tri.y[0], 1.0f, 0.0f, 0.0f});
```

### Current GPU vertex format

`navmesh_pipeline.h`:
```cpp
struct NavmeshVertex {
    float posX, posY;              // 2D only (8 bytes)
    float baryX, baryY, baryZ;    // barycentric coords (12 bytes)
};                                 // Total: 20 bytes
```

### Current shader (2D)

Vertex shader does flat 2D→NDC mapping with 4-float constant buffer:
```hlsl
o.pos.x =  (transform.y - i.pos.y) * transform.z;   // centerY - wowY scaled
o.pos.y = -(transform.x - i.pos.x) * transform.w;   // centerX - wowX scaled
o.pos.z = 0.5;  // fixed depth
o.pos.w = 1.0;
```

### Current pipeline state

- Depth buffer: **disabled** (`DepthEnable = FALSE`)
- Backface culling: **disabled** (`CullMode = D3D11_CULL_NONE`)
- Rasterizer: solid fill, scissor enabled
- Blending: alpha blending (semi-transparent navmesh)

---

## Phase 1: ViewMode Abstraction + 3D Camera System

### Goal
Introduce a `ViewMode` enum, a 3D camera class, and the UI toggle between 2D/3D. After this phase, pressing the toggle shows an empty 3D viewport with camera controls (no navmesh yet).

### Step 1.1: ViewMode enum + App integration

**File**: `src/app.h` (modify)

Add enum and state:
```cpp
enum class ViewMode { Mode2D, Mode3D };
```

Add to `App` private members:
```cpp
ViewMode m_viewMode = ViewMode::Mode2D;
Camera3D m_camera3d;
```

**File**: `src/app.cpp` (modify `RenderFrame()`)

Branch rendering based on `m_viewMode`:
```cpp
void App::RenderFrame() {
    // ... ImGui NewFrame ...

    if (m_viewMode == ViewMode::Mode2D) {
        // Existing 2D rendering code (unchanged)
        RenderFrame2D();
    } else {
        // New 3D rendering code
        RenderFrame3D();
    }

    // ... ImGui::Render(), Present ...
}
```

Extract existing `RenderFrame()` body into `RenderFrame2D()`. Create empty `RenderFrame3D()` stub.

### Step 1.2: UI toggle

**File**: `src/ui/main_menu.h/.cpp` (modify)

Add to View menu:
```
View > Mode: 2D (Ctrl+1)
View > Mode: 3D (Ctrl+2)
```

Or as a toolbar toggle button above the canvas.

**File**: `src/ui/status_bar.h/.cpp` (modify)

Show current mode: `Mode: 2D` / `Mode: 3D`

### Step 1.3: Camera3D class

**New file**: `src/camera/camera3d.h`
**New file**: `src/camera/camera3d.cpp`

```cpp
namespace mapedit {

struct Camera3D {
    // --- Position ---
    // Target point the camera orbits around (WoW world coords)
    float targetX = 0.0f;   // WoW X
    float targetY = 0.0f;   // WoW Y
    float targetZ = 100.0f; // WoW Z

    // Orbit parameters
    float distance = 200.0f;  // distance from target (yards)
    float yaw   = 0.0f;      // horizontal rotation (radians, 0=south/WoW +X)
    float pitch  = -0.6f;    // vertical angle (radians, negative=looking down)

    // Limits
    static constexpr float kMinDist  = 10.0f;
    static constexpr float kMaxDist  = 5000.0f;
    static constexpr float kMinPitch = -1.5f;  // nearly straight down
    static constexpr float kMaxPitch = -0.05f;  // nearly horizontal

    // Viewport (set each frame)
    float vpX = 0.0f, vpY = 0.0f;
    float vpW = 0.0f, vpH = 0.0f;

    // Near/far planes
    float nearPlane = 1.0f;
    float farPlane  = 10000.0f;
    float fovY      = 60.0f; // degrees

    // Camera position (computed from target + orbit params)
    float eyeX = 0.0f, eyeY = 0.0f, eyeZ = 0.0f;

    // --- Methods ---

    // Compute View and Projection matrices (row-major for HLSL)
    // Updates eyeX/Y/Z as side effect
    void ComputeMatrices(float outView[16], float outProj[16]);

    // Process input: right-click drag orbit, scroll zoom, middle-click pan
    void ProcessInput();

    // World to screen (for 3D overlays like labels, circles)
    bool WorldToScreen(float wx, float wy, float wz,
                       float& sx, float& sy) const;

    // Get frustum planes for culling (6 planes)
    void GetFrustumPlanes(float planes[6][4]) const;
};

} // namespace mapedit
```

### Camera3D input mapping

| Action | Input | Behavior |
|--------|-------|----------|
| Orbit rotate | Right-click drag | `yaw += dx * sensitivity`, `pitch += dy * sensitivity` |
| Zoom | Scroll wheel | `distance *= (1 - wheel * 0.1)`, clamped |
| Pan target | Middle-click drag | Move `targetX/Y` in camera-relative horizontal plane |
| Focus tile | Double-click on tile | Set `targetX/Y/Z` to tile center, animate `distance` |

### Camera3D math

**Eye position from orbit params** (WoW coords: X+=south, Y+=west, Z+=up):
```cpp
void Camera3D::ComputeMatrices(float outView[16], float outProj[16]) {
    // Eye position = target + spherical offset
    // WoW: yaw=0 means facing south (+X), pitch negative = looking down
    eyeX = targetX + distance * cos(pitch) * cos(yaw);
    eyeY = targetY + distance * cos(pitch) * sin(yaw);
    eyeZ = targetZ + distance * (-sin(pitch)); // negative pitch = above target

    // View matrix: LookAt(eye, target, up=[0,0,1])
    // Build manually or use DirectXMath XMMatrixLookAtLH
    // NOTE: WoW uses left-handed Z-up, DX11 expects left-handed Y-up clip space
    // Solution: swap Z↔Y in the view matrix or use a world-to-camera transform

    // Projection: perspective LH
    float aspect = vpW / vpH;
    float fovRad = fovY * 3.14159f / 180.0f;
    // Build standard perspective matrix
}
```

**Critical: WoW coordinate system → DX11 clip space mapping**

WoW is Z-up (X=south, Y=west, Z=up). DX11 clip space is Y-up. Options:
1. **Recommended**: Build a custom view matrix that transforms WoW coords (X,Y,Z) into camera space where DX11's Y is up. This is just a rotation + translation in the view matrix.
2. Specifically: the "up" vector for LookAt is `(0, 0, 1)` in WoW space, and the view matrix maps that to DX11's Y-up.

**Implementation note**: Use `DirectXMath` (`XMMatrixLookAtLH`, `XMMatrixPerspectiveFovLH`) which is header-only and ships with Windows SDK (already linked via DX11). Transform WoW coords `(X, Y, Z)` into DX11 by swapping: `dxPos = (wowY, wowZ, wowX)` (Y→X, Z→Y, X→Z) to get a right-oriented system, OR build the LookAt with WoW's Z-up convention and let the matrix handle it.

### Step 1.4: Depth buffer creation

**File**: `src/app.h` (add member)
```cpp
ID3D11DepthStencilView* m_dsv = nullptr;
ID3D11Texture2D*        m_depthTex = nullptr;
```

**File**: `src/app.cpp` (modify `CreateRenderTarget`)

Create depth-stencil buffer alongside the render target:
```cpp
// After creating the backbuffer RTV:
D3D11_TEXTURE2D_DESC dd = {};
dd.Width     = width;
dd.Height    = height;
dd.MipLevels = 1;
dd.ArraySize = 1;
dd.Format    = DXGI_FORMAT_D24_UNORM_S8_UINT;
dd.SampleDesc.Count = 1;
dd.Usage     = D3D11_USAGE_DEFAULT;
dd.BindFlags = D3D11_BIND_DEPTH_STENCIL;
m_device->CreateTexture2D(&dd, nullptr, &m_depthTex);
m_device->CreateDepthStencilView(m_depthTex, nullptr, &m_dsv);
```

**IMPORTANT**: Only bind the depth buffer in 3D mode. 2D mode continues with `nullptr` DSV (no depth). This ensures zero change to existing 2D behavior.

### Step 1.5: RenderFrame3D stub

```cpp
void App::RenderFrame3D() {
    // Clear depth buffer
    m_context->ClearDepthStencilView(m_dsv, D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.0f, 0);

    // Bind RTV + DSV
    m_context->OMSetRenderTargets(1, &m_rtv, m_dsv);

    // Camera
    m_camera3d.vpX = ...; // from ImGui workspace
    m_camera3d.vpW = ...;
    m_camera3d.ProcessInput();

    float view[16], proj[16];
    m_camera3d.ComputeMatrices(view, proj);

    // (Phase 2: render navmesh here)
    // (Phase 3: render overlays here)

    // Reset RTV without DSV for ImGui UI overlay
    m_context->OMSetRenderTargets(1, &m_rtv, nullptr);

    // Render ImGui panels (same panels as 2D: layer panel, property panel, etc.)
    // But NOT the 2D canvas/renderers
    m_mainMenu.Render(...);
    m_layerPanel.Render(...);
    m_propertyPanel.Render(...);
    m_statusBar.Render3D(m_camera3d, ...);
    m_logWindow.Render();
}
```

### Files changed/created in Phase 1

```
NEW:  src/camera/camera3d.h
NEW:  src/camera/camera3d.cpp
MOD:  src/app.h          — add ViewMode, Camera3D, depth buffer members
MOD:  src/app.cpp         — split RenderFrame, add RenderFrame2D/3D, depth buffer
MOD:  src/ui/main_menu.h  — add mode toggle
MOD:  src/ui/main_menu.cpp
MOD:  src/ui/status_bar.h  — show mode
MOD:  src/ui/status_bar.cpp
```

### Verification

- Launch app → default 2D mode (no change)
- Press Ctrl+2 or click toggle → switch to 3D mode → see dark empty viewport
- Right-click drag → orbit (camera moves)
- Scroll → zoom in/out
- Middle-click drag → pan
- Press Ctrl+1 → back to 2D mode (unchanged)
- Status bar shows `Mode: 3D`

---

## Phase 2: 3D Navmesh GPU Pipeline + Renderer

### Goal

Render navmesh triangles in 3D with proper perspective, depth, and wireframe overlay. Reuse the existing TileCache/TileLoader data pipeline — only change the GPU upload and shading.

### Step 2.1: 3D vertex format

**New file**: `src/render/navmesh_pipeline_3d.h`

```cpp
namespace mapedit {

// GPU vertex for 3D navmesh: world XYZ + barycentric coords
struct NavmeshVertex3D {
    float posX, posY, posZ;        // WoW world coords (12 bytes)
    float baryX, baryY, baryZ;    // barycentric coords (12 bytes)
};                                  // Total: 24 bytes

// Constant buffer for 3D navmesh (must be 16-byte aligned)
struct NavmeshCB3D {
    float viewProj[16];  // 4x4 View*Projection matrix (64 bytes)
    float fillColor[4];  // RGBA (16 bytes)
    float edgeColor[4];  // RGBA (16 bytes)
    float lightDir[4];   // directional light (16 bytes), w=unused
};                        // Total: 112 bytes

class NavmeshPipeline3D {
public:
    bool Initialize(ID3D11Device* device);
    void Shutdown();

    // Accessors (same pattern as NavmeshPipeline)
    ID3D11VertexShader*      GetVS()            const;
    ID3D11PixelShader*       GetPS()            const;
    ID3D11InputLayout*       GetInputLayout()   const;
    ID3D11BlendState*        GetBlendState()    const;
    ID3D11RasterizerState*   GetRastState()     const;
    ID3D11DepthStencilState* GetDSState()       const;
    ID3D11Buffer*            GetConstantBuffer() const;
    bool IsReady() const;

private:
    // Same members as NavmeshPipeline
    ID3D11VertexShader*      m_vs = nullptr;
    ID3D11PixelShader*       m_ps = nullptr;
    ID3D11InputLayout*       m_inputLayout = nullptr;
    ID3D11BlendState*        m_blendState = nullptr;
    ID3D11RasterizerState*   m_rastState = nullptr;
    ID3D11DepthStencilState* m_dsState = nullptr;
    ID3D11Buffer*            m_cb = nullptr;
};

} // namespace mapedit
```

### Step 2.2: 3D HLSL shaders

**New file**: `src/render/navmesh_pipeline_3d.cpp`

**Vertex shader**:
```hlsl
cbuffer NavmeshCB3D : register(b0) {
    float4x4 viewProj;
    float4 fillColor;
    float4 edgeColor;
    float4 lightDir;
};

struct VS_IN  { float3 pos : POSITION; float3 bary : TEXCOORD0; };
struct VS_OUT {
    float4 clipPos : SV_POSITION;
    float3 bary    : TEXCOORD0;
    float3 worldPos: TEXCOORD1;  // for lighting normal computation
};

VS_OUT main(VS_IN i) {
    VS_OUT o;
    // WoW coords (X=south, Y=west, Z=up) → DX11 clip space
    // The viewProj matrix already handles the coordinate transform
    float4 worldPos = float4(i.pos.x, i.pos.y, i.pos.z, 1.0);
    o.clipPos  = mul(worldPos, viewProj);
    o.bary     = i.bary;
    o.worldPos = i.pos;
    return o;
}
```

**Pixel shader** (barycentric wireframe + simple directional lighting):
```hlsl
cbuffer NavmeshCB3D : register(b0) {
    float4x4 viewProj;
    float4 fillColor;
    float4 edgeColor;
    float4 lightDir;
};

struct PS_IN {
    float4 clipPos  : SV_POSITION;
    float3 bary     : TEXCOORD0;
    float3 worldPos : TEXCOORD1;
};

float4 main(PS_IN i) : SV_TARGET {
    // Wireframe via barycentric coords (same as 2D)
    float3 d = fwidth(i.bary);
    float3 s = smoothstep(float3(0,0,0), d * 1.5, i.bary);
    float  w = min(s.x, min(s.y, s.z));
    float4 baseColor = lerp(edgeColor, fillColor, w);

    // Flat normal from derivatives (per-triangle)
    float3 dpdx = ddx(i.worldPos);
    float3 dpdy = ddy(i.worldPos);
    float3 N = normalize(cross(dpdx, dpdy));

    // Simple directional light
    float NdotL = max(dot(N, lightDir.xyz), 0.0);
    float ambient = 0.3;
    float lighting = ambient + (1.0 - ambient) * NdotL;

    return float4(baseColor.rgb * lighting, baseColor.a);
}
```

### Step 2.3: Pipeline state differences from 2D

| State | 2D pipeline | 3D pipeline |
|-------|------------|-------------|
| Input layout | `float2 POSITION` + `float3 TEXCOORD0` | `float3 POSITION` + `float3 TEXCOORD0` |
| Constant buffer | 48 bytes (transform + colors) | 112 bytes (viewProj + colors + light) |
| Depth-stencil | **Disabled** | **Enabled** (`DepthEnable=TRUE, DepthFunc=LESS`) |
| Backface culling | None | `D3D11_CULL_BACK` (or keep NONE for two-sided viewing) |
| Blend | Alpha blend (semi-transparent) | Opaque or slight alpha |
| Rasterizer | Scissor, no depth clip | Standard depth clip, optional wireframe mode |

### Step 2.4: 3D Navmesh Renderer

**New file**: `src/render/navmesh_renderer_3d.h`
**New file**: `src/render/navmesh_renderer_3d.cpp`

Structure mirrors `NavmeshRenderer` but with 3D pipeline:

```cpp
class NavmeshRenderer3D {
public:
    bool Initialize(ID3D11Device* device, ID3D11DeviceContext* context);
    void Shutdown();

    // Render all visible navmesh tiles in 3D
    void Render(const Camera3D& camera, TileCache& cache,
                ID3D11DepthStencilView* dsv);

private:
    using TileKey = std::pair<int, int>;

    struct TileGpuData3D {
        ID3D11Buffer* detailVB = nullptr;
        UINT detailVertCount = 0;
        ID3D11Buffer* baseVB = nullptr;
        UINT baseVertCount = 0;
        // 3D AABB for frustum culling
        float minX, minY, minZ;
        float maxX, maxY, maxZ;
    };

    void SyncGpuCache(TileCache& cache);
    void ReleaseGpuTile(TileGpuData3D& tile);
    ID3D11Buffer* CreateVB3D(const TileTriangles& tris, bool detail);

    NavmeshPipeline3D m_pipeline;
    std::map<TileKey, TileGpuData3D> m_gpuCache;
    // ... same pattern as 2D renderer
};
```

**Key difference in `BuildVertexData3D`** — include Z:
```cpp
static void BuildVertexData3D(const TileTriangles& tris,
                               std::vector<NavmeshVertex3D>& out) {
    out.clear();
    out.reserve(tris.triangles.size() * 3);
    for (const auto& tri : tris.triangles) {
        out.push_back({tri.x[0], tri.y[0], tri.z[0], 1.0f, 0.0f, 0.0f});
        out.push_back({tri.x[1], tri.y[1], tri.z[1], 0.0f, 1.0f, 0.0f});
        out.push_back({tri.x[2], tri.y[2], tri.z[2], 0.0f, 0.0f, 1.0f});
    }
}
```

### Step 2.5: 3D Render call (not via ImGui callback)

Unlike the 2D renderer which injects into ImGui's draw list via `AddCallback`, the 3D renderer issues DX11 draw calls **directly** (not through ImGui), because:
- ImGui continues to render UI panels as 2D overlay
- The 3D scene is rendered to the backbuffer+depth before ImGui

```cpp
void NavmeshRenderer3D::Render(const Camera3D& camera, TileCache& cache,
                                ID3D11DepthStencilView* dsv) {
    SyncGpuCache(cache);
    if (m_gpuCache.empty()) return;

    // Set pipeline state
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->VSSetShader(m_pipeline.GetVS(), ...);
    ctx->PSSetShader(m_pipeline.GetPS(), ...);
    ctx->IASetInputLayout(m_pipeline.GetInputLayout());
    ctx->OMSetDepthStencilState(m_pipeline.GetDSState(), 0);
    ctx->RSSetState(m_pipeline.GetRastState());
    ctx->OMSetBlendState(m_pipeline.GetBlendState(), ...);

    // Update constant buffer with VP matrix
    float view[16], proj[16];
    camera.ComputeMatrices(view, proj);
    NavmeshCB3D cb = {};
    // cb.viewProj = view * proj (multiply manually or via DirectXMath)
    MultiplyMatrix4x4(view, proj, cb.viewProj);
    cb.fillColor = {0.0f, 0.7f, 0.3f, 0.9f};
    cb.edgeColor = {0.0f, 0.85f, 0.4f, 1.0f};
    cb.lightDir  = {-0.5f, -0.3f, 0.8f, 0.0f}; // from above-right
    // normalize lightDir
    // Map + copy CB

    // Frustum cull + draw visible tiles
    float frustum[6][4];
    camera.GetFrustumPlanes(frustum);

    UINT stride = sizeof(NavmeshVertex3D);
    UINT offset = 0;
    for (auto& [key, tile] : m_gpuCache) {
        if (!FrustumIntersectsAABB(frustum, tile)) continue;
        ID3D11Buffer* vb = tile.detailVB; // or LOD selection based on distance
        if (!vb) continue;
        ctx->IASetVertexBuffers(0, 1, &vb, &stride, &offset);
        ctx->Draw(tile.detailVertCount, 0);
    }
}
```

### Step 2.6: TileCache viewport update for 3D

`TileCache::UpdateViewport()` currently takes a 2D canvas. For 3D, it needs the camera's target position and frustum.

**Option A (simpler)**: add a second update method:
```cpp
// Loads tiles near the camera target in a radius proportional to distance
void TileCache::UpdateViewport3D(float targetX, float targetY, float cameraDistance);
```

This computes a loading radius from `cameraDistance` and loads tiles in that area, using the same spiral-nearest-first pattern.

**Option B**: extract tile bounds from frustum projection onto XY plane.

**Recommendation**: Option A for simplicity. The camera target is always the area of interest.

### Files changed/created in Phase 2

```
NEW:  src/render/navmesh_pipeline_3d.h
NEW:  src/render/navmesh_pipeline_3d.cpp
NEW:  src/render/navmesh_renderer_3d.h
NEW:  src/render/navmesh_renderer_3d.cpp
MOD:  src/app.h           — add NavmeshRenderer3D member
MOD:  src/app.cpp          — call 3D renderer in RenderFrame3D
MOD:  src/navmesh/tile_cache.h  — add UpdateViewport3D
MOD:  src/navmesh/tile_cache.cpp
```

### Verification

- Switch to 3D mode → see navmesh triangles with elevation
- Orbit camera → terrain shape visible (hills, valleys, cliffs)
- Wireframe edges visible on triangles
- Zoom in/out → tiles load/unload smoothly
- Depth sorting correct (no z-fighting, nearer polygons occlude farther)
- Switch back to 2D → unchanged behavior

---

## Phase 3: 3D Overlays (Graph, Routes, Path, Grid)

### Goal

Render the same data layers as 2D (graph nodes/edges, routes, pathfinding results, grid) but in 3D space. Nodes become spheres or billboarded circles, edges/routes become 3D polylines with Y offset.

### Step 3.1: 3D line/point rendering utility

**New file**: `src/render/primitives_3d.h`
**New file**: `src/render/primitives_3d.cpp`

A shared utility for drawing 3D primitives:

```cpp
class Primitives3D {
public:
    bool Initialize(ID3D11Device* device);
    void Shutdown();

    // Begin a new frame (resets dynamic VB write position)
    void BeginFrame();

    // Add line segment in 3D world space
    void AddLine(float x1, float y1, float z1,
                 float x2, float y2, float z2,
                 uint32_t color, float thickness = 1.0f);

    // Add circle (billboard facing camera) at world position
    void AddCircle(float wx, float wy, float wz,
                   float radius, uint32_t color, int segments = 16);

    // Add sphere wireframe
    void AddSphere(float wx, float wy, float wz,
                   float radius, uint32_t color);

    // Flush all accumulated geometry
    void Flush(ID3D11DeviceContext* ctx, const float viewProj[16]);

private:
    // Dynamic VB for lines (position + color)
    struct LineVertex {
        float x, y, z;
        uint32_t color; // ABGR packed
    };

    ID3D11Device* m_device = nullptr;
    ID3D11Buffer* m_lineVB = nullptr;    // dynamic, large enough for frame
    ID3D11VertexShader* m_lineVS = nullptr;
    ID3D11PixelShader*  m_linePS = nullptr;
    ID3D11InputLayout*  m_lineLayout = nullptr;
    // ...
    std::vector<LineVertex> m_lines;
};
```

**Line shader** (simple 3D with vertex color):
```hlsl
// VS
cbuffer CB : register(b0) { float4x4 viewProj; };
struct VS_IN  { float3 pos : POSITION; float4 col : COLOR; };
struct VS_OUT { float4 pos : SV_POSITION; float4 col : COLOR; };
VS_OUT main(VS_IN i) {
    VS_OUT o;
    o.pos = mul(float4(i.pos, 1.0), viewProj);
    o.col = i.col;
    return o;
}

// PS
float4 main(PS_IN i) : SV_TARGET { return i.col; }
```

### Step 3.2: Graph3DRenderer

**New file**: `src/render/graph_renderer_3d.h`
**New file**: `src/render/graph_renderer_3d.cpp`

```cpp
class Graph3DRenderer {
public:
    void Render(const Camera3D& camera, Primitives3D& prims,
                const WorldGraphData& graph, uint32_t mapId,
                const Selection& selection);
};
```

Nodes: Draw as small spheres or billboard circles at `(node.x, node.y, node.z)` in 3D.
Edges: Draw as 3D lines between node positions, slightly elevated (+0.5 Z) to avoid z-fighting with navmesh.
Selection: Brighter color, larger radius.
Labels: Use `Camera3D::WorldToScreen()` to project labels to screen coordinates, then draw via ImGui `GetForegroundDrawList()->AddText()`.

### Step 3.3: Route3DRenderer

**New file**: `src/render/route_renderer_3d.h`
**New file**: `src/render/route_renderer_3d.cpp`

Waypoints as spheres, connecting lines as 3D polylines. Numbers as billboard text (project to screen via `WorldToScreen`).

### Step 3.4: Path3DRenderer

**New file**: `src/render/path_renderer_3d.h`
**New file**: `src/render/path_renderer_3d.cpp`

Same as 2D path renderer but in 3D. Points A/B as colored spheres. Path line elevated +0.3Z above navmesh to be visible.

**Pathfinding Z**: Currently `PathResult` stores 2D waypoints `(wowX, wowY)`. Extend to include Z from Detour's `findStraightPath` output (`straightPath[i*3+1]` = Detour Y = WoW Z). This allows the path to follow terrain elevation.

### Step 3.5: Grid3DRenderer

**New file**: `src/render/grid_renderer_3d.h`
**New file**: `src/render/grid_renderer_3d.cpp`

Draw coordinate grid lines at Z=0 (flat reference plane). Tile boundaries as 3D rectangles at Z=0. Faded/transparent.

Optional: draw tile boundaries at navmesh height (sample Z from navmesh at tile edges).

### Step 3.6: 3D text/labels via ImGui overlay

For text labels (node names, distance labels, waypoint numbers), project 3D positions to screen coords using `Camera3D::WorldToScreen()` and render via ImGui's `GetForegroundDrawList()`. This avoids the complexity of 3D text rendering entirely.

```cpp
float sx, sy;
if (camera.WorldToScreen(node.x, node.y, node.z, sx, sy)) {
    ImGui::GetForegroundDrawList()->AddText(
        ImVec2(sx + 8, sy - 6), IM_COL32(255,255,255,200), node.name.c_str());
}
```

### Files changed/created in Phase 3

```
NEW:  src/render/primitives_3d.h
NEW:  src/render/primitives_3d.cpp
NEW:  src/render/graph_renderer_3d.h
NEW:  src/render/graph_renderer_3d.cpp
NEW:  src/render/route_renderer_3d.h
NEW:  src/render/route_renderer_3d.cpp
NEW:  src/render/path_renderer_3d.h
NEW:  src/render/path_renderer_3d.cpp
NEW:  src/render/grid_renderer_3d.h
NEW:  src/render/grid_renderer_3d.cpp
MOD:  src/app.h            — add 3D renderer members
MOD:  src/app.cpp           — call 3D renderers in RenderFrame3D
MOD:  src/navmesh/pathfinder.h  — add Z to PathResult waypoints
MOD:  src/navmesh/pathfinder.cpp
```

### Verification

- Switch to 3D → see navmesh + graph nodes floating at correct heights
- Node labels appear near nodes (screen-projected)
- Route waypoints connected by 3D polylines following terrain
- Pathfinding result line follows terrain elevation
- Grid visible at ground level
- Selection highlighting works in 3D

---

## Phase 4: Player Emulation (Movement Along Paths)

### Goal

Place a "player" marker in 3D space that can move along pathfinding routes, simulating WoW movement. Third-person camera follows the player.

### Step 4.1: PlayerMarker

**New file**: `src/simulation/player_marker.h`
**New file**: `src/simulation/player_marker.cpp`

```cpp
namespace mapedit {

class PlayerMarker {
public:
    // Current position in WoW world coords
    float posX = 0.0f, posY = 0.0f, posZ = 0.0f;
    float facing = 0.0f; // radians (WoW convention: 0=south)

    // Movement speed (yards per second)
    float speed = 7.0f; // WoW default run speed

    // Pathfinding route to follow
    struct RoutePoint { float x, y, z; };
    std::vector<RoutePoint> route;
    int currentWaypointIndex = 0;
    bool isMoving = false;
    bool loop = false;

    // Set a pathfinding destination (uses Detour to compute route)
    void SetDestination(float destX, float destY, float destZ,
                        MapPathfinder& pf, dtNavMeshQuery* query);

    // Follow an existing route (from RouteData)
    void FollowRoute(const std::vector<RoutePoint>& waypoints, bool looped);

    // Update position each frame (advance along route)
    void Update(float deltaTime);

    // Stop movement
    void Stop();

    // Is the player currently on a route?
    bool IsMoving() const { return isMoving; }

    // Place player at a position (click to teleport)
    void SetPosition(float x, float y, float z);
};

} // namespace mapedit
```

### Step 4.2: Movement update logic

```cpp
void PlayerMarker::Update(float deltaTime) {
    if (!isMoving || route.empty()) return;
    if (currentWaypointIndex >= (int)route.size()) {
        if (loop) {
            currentWaypointIndex = 0;
        } else {
            isMoving = false;
            return;
        }
    }

    auto& target = route[currentWaypointIndex];
    float dx = target.x - posX;
    float dy = target.y - posY;
    float dz = target.z - posZ;
    float dist = sqrtf(dx*dx + dy*dy + dz*dz);

    float step = speed * deltaTime;

    if (step >= dist) {
        // Arrived at waypoint
        posX = target.x;
        posY = target.y;
        posZ = target.z;
        currentWaypointIndex++;
        // Recurse with remaining time
        float remaining = (step - dist) / speed;
        if (remaining > 0.001f) Update(remaining);
    } else {
        // Move toward waypoint
        float t = step / dist;
        posX += dx * t;
        posY += dy * t;
        posZ += dz * t;
        // Update facing toward movement direction
        facing = atan2f(dy, dx); // WoW facing convention
    }
}
```

### Step 4.3: Camera follow mode

Add a follow mode to `Camera3D`:

```cpp
struct Camera3D {
    // ... existing orbit fields ...

    // Follow mode
    bool followMode = false;
    float followOffsetX = 0.0f; // behind player
    float followOffsetZ = 5.0f; // above player
    float followDistance = 15.0f;

    void UpdateFollow(const PlayerMarker& player) {
        if (!followMode) return;
        targetX = player.posX;
        targetY = player.posY;
        targetZ = player.posZ + 2.0f; // slightly above feet
        // Optionally auto-orient yaw to face player's facing direction
    }
};
```

### Step 4.4: Player rendering

The player marker is drawn as:
- A vertical capsule/cylinder (simple wireframe via `Primitives3D`)
- Or a cone/arrow pointing in the `facing` direction
- Or just a sphere + direction line

Implementation via `Primitives3D::AddSphere()` + `Primitives3D::AddLine()` for facing direction.

### Step 4.5: Player interaction UI

Add to the 3D mode:
- **Click to place**: Ctrl+Click on navmesh → teleport player to point
- **Click to move**: Right-click on navmesh → pathfind + start moving
- **Route follow**: Select a route in route editor → "Follow Route" button
- **Speed slider**: 1x, 2x, 5x, 10x multiplier
- **Stop button**: Ctrl+S or toolbar button

### Step 4.6: Navmesh hit testing (3D mouse → world)

To click on the navmesh in 3D, we need ray-mesh intersection:

```cpp
// Unproject screen coords to world ray
void Camera3D::ScreenToRay(float sx, float sy,
                            float& originX, float& originY, float& originZ,
                            float& dirX, float& dirY, float& dirZ) const;

// Find where the ray hits the navmesh
bool RaycastNavmesh(dtNavMeshQuery* query,
                    float originX, float originY, float originZ,
                    float dirX, float dirY, float dirZ,
                    float& hitX, float& hitY, float& hitZ);
```

**Approximate approach**: cast the ray downward from a high Z. Find the nearest poly at the XY intersection point, then get the Z from the navmesh.

**Better approach**: Use `dtNavMeshQuery::raycast()` with a start position high above the clicked point and endpoint at ground level.

### Files changed/created in Phase 4

```
NEW:  src/simulation/player_marker.h
NEW:  src/simulation/player_marker.cpp
MOD:  src/camera/camera3d.h    — add follow mode, ScreenToRay
MOD:  src/camera/camera3d.cpp
MOD:  src/app.h                — add PlayerMarker member
MOD:  src/app.cpp              — update + render player in RenderFrame3D
MOD:  src/ui/property_panel.h  — player controls (speed, follow route)
MOD:  src/ui/property_panel.cpp
```

### Verification

- 3D mode → Ctrl+click on navmesh → player sphere appears at clicked point
- Right-click elsewhere → player pathfinds and moves along route
- Player follows terrain elevation (goes up/down hills)
- Camera follows player in follow mode
- Speed slider works (2x, 5x = faster movement)
- Route follow: select route → click Follow → player walks the route
- Loop mode: player repeats route indefinitely
- Stop button halts movement

---

## Phase 5: Polish (Lighting, Height Coloring, LOD, UX)

### Goal

Visual improvements and optimizations to make the 3D mode useful for navigation analysis.

### Step 5.1: Height-based navmesh coloring

Color navmesh triangles by elevation (Z coordinate). This makes terrain shape more visible without textures.

**Approach**: Add a `heightColorMode` flag. When enabled, the pixel shader computes color from worldPos.z:

```hlsl
// In pixel shader
float heightNorm = saturate((i.worldPos.z - heightMin) / (heightMax - heightMin));
float3 heightColor = lerp(float3(0.0, 0.3, 0.6), float3(0.9, 0.95, 1.0), heightNorm);
// Low = deep blue, High = white (like a topographic map)
```

Pass `heightMin` / `heightMax` via constant buffer (compute from loaded tile Z range).

### Step 5.2: Multiple color modes

| Mode | Description |
|------|-------------|
| Flat green | Same as 2D (single fill + edge color) |
| Height gradient | Blue(low) → Green(mid) → White(high) |
| Slope shading | Flat=green, steep=red (useful for finding cliffs) |
| Tile-colored | Each tile a different hue (useful for tile boundary debugging) |

Toggle via Layer Panel.

### Step 5.3: LOD by distance

The 2D renderer already has detail/base VB per tile. Extend to 3D:
- **Near** (< 200 yards from camera): detail mesh (full tessellation)
- **Far** (> 200 yards): base polygons (fewer triangles)
- **Very far** (> 1000 yards): don't render (or use a flat colored quad per tile)

### Step 5.4: Wireframe toggle

Toggle navmesh wireframe edges on/off (already supported via `drawEdges` flag — expose to Layer Panel for 3D).

### Step 5.5: Minimap ground plane

Render minimap tile textures (already loaded by `MinimapTileCache`) as textured quads at Z=0 beneath the navmesh. This provides spatial reference without full terrain.

```cpp
// For each minimap tile: draw a textured quad at Z=0
// Vertices: (tileMinX, tileMinY, 0), (tileMaxX, tileMinY, 0), etc.
// UV: (0,0) → (1,1)
// Use the existing SRV textures from MinimapTileCache
```

Requires a simple textured quad shader (pos3D + UV → sample texture).

### Step 5.6: Keyboard shortcuts

| Key | Action |
|-----|--------|
| `1` | Switch to 2D mode |
| `2` | Switch to 3D mode |
| `F` | Focus camera on selection (center on selected node/waypoint) |
| `C` | Toggle follow mode on/off |
| `Space` | Pause/resume player movement |
| `+` / `-` | Speed up / slow down player |
| `G` | Toggle grid |
| `N` | Toggle navmesh |

### Step 5.7: Smooth camera transitions

When switching 2D→3D, interpolate camera position:
- 2D center (X,Y) → 3D target (X,Y, navmeshZ)
- 2D zoom → 3D distance (approximate: `distance = viewportHeight / zoom / 2`)

When focusing on a node: animate camera target from current to node position over ~0.5s.

### Files changed/created in Phase 5

```
MOD:  src/render/navmesh_pipeline_3d.h   — add height color uniforms
MOD:  src/render/navmesh_pipeline_3d.cpp — update shaders
MOD:  src/render/navmesh_renderer_3d.cpp — LOD selection
MOD:  src/ui/layer_panel.h              — 3D-specific toggles
MOD:  src/ui/layer_panel.cpp
MOD:  src/camera/camera3d.h             — smooth transitions
MOD:  src/camera/camera3d.cpp
NEW:  src/render/ground_plane_3d.h      — minimap texture ground
NEW:  src/render/ground_plane_3d.cpp
```

---

## Complete File Inventory

### New files (to create)

```
src/camera/camera3d.h
src/camera/camera3d.cpp
src/render/navmesh_pipeline_3d.h
src/render/navmesh_pipeline_3d.cpp
src/render/navmesh_renderer_3d.h
src/render/navmesh_renderer_3d.cpp
src/render/primitives_3d.h
src/render/primitives_3d.cpp
src/render/graph_renderer_3d.h
src/render/graph_renderer_3d.cpp
src/render/route_renderer_3d.h
src/render/route_renderer_3d.cpp
src/render/path_renderer_3d.h
src/render/path_renderer_3d.cpp
src/render/grid_renderer_3d.h
src/render/grid_renderer_3d.cpp
src/render/ground_plane_3d.h
src/render/ground_plane_3d.cpp
src/simulation/player_marker.h
src/simulation/player_marker.cpp
```

**Total new files**: ~20 (10 .h + 10 .cpp)

### Modified files

```
src/app.h
src/app.cpp
src/ui/main_menu.h / .cpp
src/ui/status_bar.h / .cpp
src/ui/layer_panel.h / .cpp
src/ui/property_panel.h / .cpp
src/navmesh/tile_cache.h / .cpp
src/navmesh/pathfinder.h / .cpp
```

**Total modified files**: ~16

### No changes needed

```
src/canvas/canvas.h / .cpp           — 2D only, untouched
src/render/navmesh_pipeline.h / .cpp — 2D pipeline, untouched
src/render/navmesh_renderer.h / .cpp — 2D renderer, untouched
src/render/graph_renderer.h / .cpp   — 2D only
src/render/route_renderer.h / .cpp   — 2D only
src/render/path_renderer.h / .cpp    — 2D only
src/render/grid_renderer.h / .cpp    — 2D only
src/navmesh/tile_loader.h / .cpp     — already extracts Z
src/data/*                           — shared data layer, no rendering
src/editor/*                         — shared editing logic
src/mpq/*                            — shared MPQ access
```

---

## Dependencies

No new vcpkg packages needed. `DirectXMath` (for matrix operations) is header-only and ships with Windows SDK (already linked via `d3d11.lib`).

```cpp
#include <DirectXMath.h>
using namespace DirectX;
```

---

## Critical Implementation Notes

### 1. WoW → DX11 coordinate mapping

WoW: X+=south, Y+=west, Z+=up (left-handed, Z-up).
DX11 clip space: X+=right, Y+=up, Z+=into screen (left-handed, Y-up).

**Solution**: In the view matrix construction, treat WoW coords as `(X, Y, Z)` and build LookAt with `up = (0, 0, 1)`. The projection matrix will handle the rest. Use `DirectXMath` with `XMMatrixLookAtLH`:

```cpp
XMVECTOR eye    = XMVectorSet(eyeX, eyeY, eyeZ, 1.0f);
XMVECTOR target = XMVectorSet(targetX, targetY, targetZ, 1.0f);
XMVECTOR up     = XMVectorSet(0, 0, 1, 0); // WoW Z-up

// This creates a view matrix that maps WoW Z-up to DX11 Y-up internally
XMMATRIX view = XMMatrixLookAtLH(eye, target, up);
XMMATRIX proj = XMMatrixPerspectiveFovLH(fovRad, aspect, nearPlane, farPlane);
XMMATRIX vp   = view * proj;

// Store as row-major float[16] for HLSL (HLSL default is column-major,
// so either transpose here or use `row_major` in HLSL)
XMStoreFloat4x4((XMFLOAT4X4*)outViewProj, XMMatrixTranspose(vp));
```

**HLSL**: Use `row_major float4x4` or transpose before upload.

### 2. ImGui overlay in 3D mode

ImGui panels (layer panel, property panel, status bar, log window, main menu) render as 2D overlay ON TOP of the 3D scene. This works naturally because:
1. Render 3D scene to backbuffer + depth buffer
2. Reset render target to `(rtv, nullptr)` — no depth
3. Call `ImGui::Render()` + `ImGui_ImplDX11_RenderDrawData()` — ImGui draws on top

### 3. Tile streaming in 3D

The existing `TileCache::UpdateViewport()` works with a 2D bounding box. For 3D, the simplest approach is to compute which tiles the camera can see by projecting the view frustum onto the XY plane and using the same tile-loading logic.

### 4. No changes to tile_loader.h

`TileTriangle` already stores `z[3]` — the Z coordinates are extracted from Detour's detail mesh but discarded in `BuildVertexData()`. The 3D renderer simply uses them.

### 5. Performance expectations

- Each navmesh tile: ~500-5000 triangles × 24 bytes/vert × 3 verts = ~36-360 KB/tile
- 200 tiles loaded: ~7-72 MB GPU memory (acceptable)
- Draw calls: 200 tiles = 200 draw calls (fine for DX11)
- Main bottleneck: pixel fill rate for large navmesh areas — mitigated by frustum culling and LOD

---

## Implementation Order (Recommended)

```
Phase 1 (camera + mode toggle)
    ├── Step 1.1: ViewMode enum in app
    ├── Step 1.2: UI toggle (menu + shortcut)
    ├── Step 1.3: Camera3D class
    ├── Step 1.4: Depth buffer
    └── Step 1.5: RenderFrame3D stub
         ↓
Phase 2 (3D navmesh rendering)
    ├── Step 2.1: NavmeshVertex3D format
    ├── Step 2.2: 3D HLSL shaders
    ├── Step 2.3: NavmeshPipeline3D
    ├── Step 2.4: NavmeshRenderer3D
    ├── Step 2.5: Direct DX11 render (not ImGui callback)
    └── Step 2.6: TileCache 3D update
         ↓
Phase 3 (3D overlays)
    ├── Step 3.1: Primitives3D utility
    ├── Step 3.2: Graph3DRenderer
    ├── Step 3.3: Route3DRenderer
    ├── Step 3.4: Path3DRenderer
    ├── Step 3.5: Grid3DRenderer
    └── Step 3.6: Text labels via ImGui projection
         ↓
Phase 4 (player emulation)
    ├── Step 4.1: PlayerMarker class
    ├── Step 4.2: Movement update logic
    ├── Step 4.3: Camera follow mode
    ├── Step 4.4: Player rendering
    ├── Step 4.5: Player UI (speed, controls)
    └── Step 4.6: Navmesh hit testing (3D click)
         ↓
Phase 5 (polish)
    ├── Step 5.1: Height coloring
    ├── Step 5.2: Color modes
    ├── Step 5.3: LOD by distance
    ├── Step 5.4: Wireframe toggle
    ├── Step 5.5: Minimap ground plane
    ├── Step 5.6: Keyboard shortcuts
    └── Step 5.7: Smooth camera transitions
```

Each phase is independently testable and the app remains functional after each phase completion.
