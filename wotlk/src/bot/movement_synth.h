#pragma once
// =============================================================================
// MovementSynth — anti-detection movement humanizer.
//
// Adds subtle imperfections to bot movement to mimic human behavior:
// - Trajectory noise (Perlin-like offset to CTM targets)
// - Micro-pauses (brief stops every 20-40s)
// - Lookahead smoothing (CTM target ahead on path, not exact next WP)
// =============================================================================

#include "../game/types.h"

#include <cstdint>
#include <utility>
#include <vector>

namespace bot {

class MovementSynth {
public:
    // Call once per movement session to reset state.
    void Reset();

    // Apply trajectory noise to a CTM target position.
    // Returns a slightly offset position (still close to original).
    game::Vec3 AddNoise(const game::Vec3& target);

    // Check if it's time for a micro-pause. Returns true if the bot should
    // briefly stop (100-300ms). Call once per tick while moving.
    bool ShouldMicroPause();

    // Get micro-pause duration in ms (valid when ShouldMicroPause returns true).
    uint32_t GetPauseDurationMs() const { return m_pauseDurationMs; }

    // Compute a lookahead point along a path from current index.
    // Returns a point ~lookaheadDist yards ahead on the polyline.
    static game::Vec3 GetLookaheadPoint(const std::vector<game::Vec3>& waypoints,
                                         size_t currentIndex,
                                         const game::Vec3& currentPos,
                                         float lookaheadDist = 12.0f);

    // Snap a noisy position back onto navmesh if it drifted off.
    // Returns {snappedPos, true} if found, {original, false} if not on navmesh.
    static std::pair<game::Vec3, bool> SnapToNavmesh(const game::Vec3& pos);

private:
    // Perlin-like 1D noise (simple hash-based interpolation)
    static float Noise1D(float t);

    // Movement timing
    uint64_t m_startTime      = 0;
    uint64_t m_lastPauseTime  = 0;
    uint64_t m_nextPauseAt    = 0;     // next pause scheduled at this tick
    uint32_t m_pauseDurationMs = 0;

    // Noise phase offsets (randomized per session)
    float m_noisePhaseX = 0.0f;
    float m_noisePhaseY = 0.0f;

    // Noise amplitude (yards)
    static constexpr float kNoiseAmplitude = 0.8f;  // +/- 0.8 yard
    static constexpr float kNoiseFrequency = 0.5f;  // cycles per second

    // Micro-pause timing
    static constexpr uint32_t kMinPauseIntervalMs = 20000; // 20s
    static constexpr uint32_t kMaxPauseIntervalMs = 40000; // 40s
    static constexpr uint32_t kMinPauseDurationMs = 100;
    static constexpr uint32_t kMaxPauseDurationMs = 300;
};

} // namespace bot
