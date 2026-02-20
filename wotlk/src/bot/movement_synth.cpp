#include "movement_synth.h"
#include "../navigation/nav_mesh.h"

#include <DetourNavMesh.h>
#include <DetourNavMeshQuery.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cmath>
#include <cstdlib>

namespace bot {

// ---------------------------------------------------------------------------
// Simple hash-based 1D noise (not true Perlin, but good enough for jitter)
// ---------------------------------------------------------------------------

static uint32_t Hash(uint32_t x) {
    x = ((x >> 16) ^ x) * 0x45d9f3b;
    x = ((x >> 16) ^ x) * 0x45d9f3b;
    x = (x >> 16) ^ x;
    return x;
}

static float HashFloat(int x) {
    // Returns [-1.0, 1.0]
    return static_cast<float>(static_cast<int>(Hash(static_cast<uint32_t>(x)))) / 2147483647.0f;
}

float MovementSynth::Noise1D(float t) {
    int i = static_cast<int>(std::floor(t));
    float frac = t - static_cast<float>(i);

    // Smoothstep interpolation
    float s = frac * frac * (3.0f - 2.0f * frac);

    float a = HashFloat(i);
    float b = HashFloat(i + 1);
    return a + s * (b - a);
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void MovementSynth::Reset() {
    uint64_t now = GetTickCount64();
    m_startTime = now;
    m_lastPauseTime = now;

    // Random phase offsets so noise pattern varies per session
    m_noisePhaseX = static_cast<float>(rand() % 10000) / 10.0f;
    m_noisePhaseY = static_cast<float>(rand() % 10000) / 10.0f + 137.0f;

    // Schedule first micro-pause
    m_nextPauseAt = now + kMinPauseIntervalMs +
        static_cast<uint32_t>(rand() % (kMaxPauseIntervalMs - kMinPauseIntervalMs));
    m_pauseDurationMs = 0;
}

game::Vec3 MovementSynth::AddNoise(const game::Vec3& target) {
    uint64_t now = GetTickCount64();
    float elapsed = static_cast<float>(now - m_startTime) / 1000.0f;
    float t = elapsed * kNoiseFrequency;

    float offsetX = Noise1D(t + m_noisePhaseX) * kNoiseAmplitude;
    float offsetY = Noise1D(t + m_noisePhaseY) * kNoiseAmplitude;

    return { target.x + offsetX, target.y + offsetY, target.z };
}

bool MovementSynth::ShouldMicroPause() {
    uint64_t now = GetTickCount64();

    if (now < m_nextPauseAt)
        return false;

    // Schedule next pause
    m_lastPauseTime = now;
    m_nextPauseAt = now + kMinPauseIntervalMs +
        static_cast<uint32_t>(rand() % (kMaxPauseIntervalMs - kMinPauseIntervalMs));

    // Random pause duration
    m_pauseDurationMs = kMinPauseDurationMs +
        static_cast<uint32_t>(rand() % (kMaxPauseDurationMs - kMinPauseDurationMs));

    return true;
}

game::Vec3 MovementSynth::GetLookaheadPoint(const std::vector<game::Vec3>& waypoints,
                                              size_t currentIndex,
                                              const game::Vec3& currentPos,
                                              float lookaheadDist) {
    if (waypoints.empty() || currentIndex >= waypoints.size())
        return currentPos;

    // Walk along the path from current position, accumulating distance
    float remaining = lookaheadDist;

    game::Vec3 prev = currentPos;
    for (size_t i = currentIndex; i < waypoints.size(); ++i) {
        float segLen = prev.Distance2D(waypoints[i]);
        if (segLen <= 0.001f) {
            prev = waypoints[i];
            continue;
        }

        if (remaining <= segLen) {
            // Interpolate within this segment
            float t = remaining / segLen;
            return {
                prev.x + t * (waypoints[i].x - prev.x),
                prev.y + t * (waypoints[i].y - prev.y),
                prev.z + t * (waypoints[i].z - prev.z)
            };
        }

        remaining -= segLen;
        prev = waypoints[i];
    }

    // Ran out of path — return last waypoint
    return waypoints.back();
}

std::pair<game::Vec3, bool> MovementSynth::SnapToNavmesh(const game::Vec3& pos) {
    auto& nm = nav::NavMesh::Instance();
    auto* query = nm.GetQuery();
    if (!query)
        return {pos, false};

    // Convert WoW -> Detour coords
    float dtPos[3] = { pos.y, pos.z, pos.x };
    float extents[3] = { 2.0f, 5.0f, 2.0f };

    dtQueryFilter filter;
    filter.setIncludeFlags(0xFFFF);
    filter.setExcludeFlags(0);

    dtPolyRef ref = 0;
    float nearest[3];
    dtStatus status = query->findNearestPoly(dtPos, extents, &filter, &ref, nearest);

    if (dtStatusFailed(status) || ref == 0)
        return {pos, false};

    // Convert Detour -> WoW coords
    game::Vec3 snapped;
    snapped.x = nearest[2];
    snapped.y = nearest[0];
    snapped.z = nearest[1];
    return {snapped, true};
}

} // namespace bot
