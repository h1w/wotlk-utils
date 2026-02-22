#pragma once

#include "../render/minimap_cache.h"

namespace mapedit {

struct LayerVisibility;

class MinimapTileCache;

class LayerPanel {
public:
    void Render(LayerVisibility& layers, MapBackgroundMode& bgMode,
                MinimapTileCache* minimapCache = nullptr);
};

} // namespace mapedit
