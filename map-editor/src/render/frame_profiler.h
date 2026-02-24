#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstring>

namespace mapedit {

struct FrameProfiler {
    enum Layer { Terrain, Buildings, Navmesh, Overlays, COUNT };

    // Averaged stats for display
    float layerMs[COUNT] = {};
    float frameMs = 0;
    float fps = 0;

    // Per-frame draw call + vertex counts (set by renderers)
    int drawCalls[COUNT] = {};
    int vertices[COUNT] = {};
    int totalDrawCalls = 0;
    int totalVertices = 0;

    void Initialize() {
        LARGE_INTEGER freq;
        QueryPerformanceFrequency(&freq);
        qpcFreqMs_ = 1000.0 / static_cast<double>(freq.QuadPart);
    }

    void BeginFrame() {
        QueryPerformanceCounter(&frameStart_);
        std::memset(drawCalls, 0, sizeof(drawCalls));
        std::memset(vertices, 0, sizeof(vertices));
        totalDrawCalls = 0;
        totalVertices = 0;
    }

    LARGE_INTEGER Now() const {
        LARGE_INTEGER t;
        QueryPerformanceCounter(&t);
        return t;
    }

    void RecordLayer(Layer layer, LARGE_INTEGER start, LARGE_INTEGER end) {
        layerAccum_[layer] += Elapsed(start, end);
    }

    void EndFrame() {
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        frameAccum_ += Elapsed(frameStart_, now);
        frames_++;

        if (frames_ >= 60) {
            double inv = 1.0 / frames_;
            for (int i = 0; i < COUNT; ++i) {
                layerMs[i] = static_cast<float>(layerAccum_[i] * inv);
                layerAccum_[i] = 0;
            }
            frameMs = static_cast<float>(frameAccum_ * inv);
            fps = (frameMs > 0.001f) ? 1000.0f / frameMs : 0;
            frameAccum_ = 0;
            frames_ = 0;
        }
    }

private:
    double Elapsed(LARGE_INTEGER a, LARGE_INTEGER b) const {
        return static_cast<double>(b.QuadPart - a.QuadPart) * qpcFreqMs_;
    }

    double layerAccum_[COUNT] = {};
    double frameAccum_ = 0;
    int    frames_ = 0;
    double qpcFreqMs_ = 0;
    LARGE_INTEGER frameStart_ = {};
};

} // namespace mapedit
