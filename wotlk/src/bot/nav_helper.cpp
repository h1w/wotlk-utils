#include "nav_helper.h"
#include "threat_scanner.h"
#include "radar.h"
#include "../game/game.h"
#include "../game/movement.h"
#include "../game/world.h"
#include "../navigation/pathfinder.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <glog/logging.h>

namespace bot {

// Max distance from player to consider NPC as danger (yards).
// NPCs beyond this range don't affect practical pathfinding.
static constexpr float kMaxDangerDistance = 150.0f;

// Shared helper: collect danger zones from RadarData
static std::vector<nav::DangerZone> CollectDangerZones()
{
    std::vector<nav::DangerZone> dangers;
    auto& radar = RadarData::Instance();
    for (const auto& e : radar.GetEntries()) {
        if (e.isPlayer || e.isDead || e.isInCombat) continue;
        if (e.aggroRadiusNav <= 0.0f) continue;
        if (e.distToPlayer > kMaxDangerDistance) continue;
        if (e.reaction != game::UnitReaction::Hostile &&
            e.reaction != game::UnitReaction::Unfriendly)
            continue;
        // Pass nav radius (base 30) — pathfinder applies margin (R * 1.15 + 3.0) internally
        dangers.push_back({ e.position.x, e.position.y, e.position.z, e.aggroRadiusNav });
    }
    return dangers;
}

bool NavHelper::StartNavTo(const game::Vec3& target)
{
    m_waypoints.clear();
    m_currentIndex = 0;
    m_stuckCount = 0;
    m_blockingThreats.clear();
    m_detourWaypoints.clear();
    m_currentPathThroughDanger = false;
    m_currentDangerPolyCount = 0;
    m_reroute = RerouteState{};
    m_recoveryState = RecoveryState::None;
    m_inMicroPause = false;
    m_microPauseEnd = 0;
    m_reactionDelayActive = false;
    m_reactionDelayEnd = 0;
    m_status = Status::Idle;

    if (m_humanize)
        m_synth.Reset();

    if (!game::world::IsInGame())
        return false;

    auto player = game::GetLocalPlayer();
    if (!player)
        return false;

    game::Vec3 start = player->GetPosition();

    // Use avoidance from the start if enabled
    nav::PathResult result;
    if (m_avoidanceEnabled) {
        auto dangers = CollectDangerZones();
        if (!dangers.empty()) {
            result = nav::Pathfinder::Instance().FindPathAvoiding(start, target, dangers);
            if (result.success && result.throughDanger) {
                // No safe path exists — still use it, CheckThreats will handle blocking later
                LOG(INFO) << "[NavHelper] Initial path goes through danger (no safe detour)";
            }
        } else {
            result = nav::Pathfinder::Instance().FindPath(start, target);
        }
    } else {
        result = nav::Pathfinder::Instance().FindPath(start, target);
    }

    if (!result.success || result.waypoints.empty())
        return false;

    m_waypoints = std::move(result.waypoints);
    m_currentIndex = 0;
    m_lastPosition = start;
    m_lastStuckCheckTick = GetTickCount64();
    m_lastCTMTick = m_lastStuckCheckTick;
    m_status = Status::Moving;

    // Skip first waypoint if it's very close (it's our current position)
    if (m_waypoints.size() > 1 && start.Distance2D(m_waypoints[0]) < kArrivalThreshold)
        m_currentIndex = 1;

    IssueCTMToCurrentWP();

    LOG(INFO) << "[NavHelper] Started path with " << m_waypoints.size()
              << " waypoints (starting at index " << m_currentIndex << ")";
    return true;
}

NavHelper::Status NavHelper::Tick()
{
    if (m_status != Status::Moving)
        return m_status;

    // Recovery waiting state — stopped, periodically re-check
    if (m_recoveryState == RecoveryState::Waiting) {
        TickRecovery();
        return m_status;
    }

    uint64_t now = GetTickCount64();

    // Micro-pause: bot briefly stops to mimic human behavior
    if (m_inMicroPause) {
        if (now >= m_microPauseEnd) {
            m_inMicroPause = false;
            IssueCTMToCurrentWP();
        }
        return m_status;
    }

    auto player = game::GetLocalPlayer();
    if (!player) {
        m_status = Status::Failed;
        return m_status;
    }

    game::Vec3 myPos = player->GetPosition();
    const game::Vec3& targetWP = m_waypoints[m_currentIndex];
    float dist = myPos.Distance2D(targetWP);

    // Arrived at current waypoint?
    if (dist < kArrivalThreshold) {
        AdvanceToNext();
        return m_status;
    }

    // CTM refresh — re-issue periodically so lookahead target tracks player movement.
    // Without this, CTM reaches the lookahead point and player stops until stuck detection.
    // Use raw lookahead WITHOUT noise/snap to avoid jitter-induced circling.
    if (now - m_lastCTMTick >= kCTMRefreshMs) {
        m_lastCTMTick = now;
        RefreshCTM();
    }

    // Micro-pause check
    if (m_humanize && m_synth.ShouldMicroPause()) {
        m_inMicroPause = true;
        m_microPauseEnd = now + m_synth.GetPauseDurationMs();
        game::movement::StopCTM();
        return m_status;
    }
    // Threat checking — event-based with fallback timer + reaction delay
    if (m_avoidanceEnabled && now >= m_reroute.lastFallbackCheck + RerouteState::kFallbackMs) {
        m_reroute.lastFallbackCheck = now;
        if (ShouldReroute(now)) {
            if (!m_reactionDelayActive) {
                // Start reaction delay (200-400ms) to mimic human response time
                m_reactionDelayActive = true;
                m_reactionDelayEnd = now + kMinReactionDelayMs +
                    static_cast<uint32_t>(rand() % (kMaxReactionDelayMs - kMinReactionDelayMs));
            }
        }
    }
    if (m_reactionDelayActive && now >= m_reactionDelayEnd) {
        m_reactionDelayActive = false;
        CheckThreats();
    }

    // Stuck detection (every 3s)
    if (now - m_lastStuckCheckTick > kStuckCheckMs) {
        float moved = myPos.Distance2D(m_lastPosition);
        m_lastPosition = myPos;
        m_lastStuckCheckTick = now;

        if (moved < kStuckThreshold) {
            m_stuckCount++;
            if (m_stuckCount > kMaxStuckRetries) {
                LOG(WARNING) << "[NavHelper] Stuck after " << kMaxStuckRetries << " retries, failing";
                game::movement::StopCTM();
                m_status = Status::Failed;
                return m_status;
            }
            // Try to unstick: jump + re-issue CTM
            game::movement::Jump();
            IssueCTMToCurrentWP();
        } else {
            m_stuckCount = 0;
        }
    }

    return m_status;
}

void NavHelper::Stop()
{
    if (m_status == Status::Moving) {
        game::movement::StopCTM();
        game::movement::StopMoving();
        m_status = Status::Idle;
    }
    m_blockingThreats.clear();
    m_detourWaypoints.clear();
    m_recoveryState = RecoveryState::None;
}

void NavHelper::AdvanceToNext()
{
    m_currentIndex++;

    if (m_currentIndex >= m_waypoints.size()) {
        game::movement::StopCTM();
        m_status = Status::Arrived;
        return;
    }

    m_stuckCount = 0;
    m_lastCTMTick = GetTickCount64();
    IssueCTMToCurrentWP();
}

void NavHelper::IssueCTMToCurrentWP()
{
    if (m_currentIndex >= m_waypoints.size())
        return;

    if (m_humanize) {
        auto player = game::GetLocalPlayer();
        game::Vec3 myPos = player ? player->GetPosition() : m_waypoints[m_currentIndex];
        game::Vec3 target = MovementSynth::GetLookaheadPoint(
            m_waypoints, m_currentIndex, myPos, kLookaheadDist);
        target = m_synth.AddNoise(target);
        // Snap noisy target back onto navmesh to avoid walking off edges
        auto [snapped, ok] = MovementSynth::SnapToNavmesh(target);
        if (ok) target = snapped;
        game::movement::ClickToMove(target);
    } else {
        game::movement::ClickToMove(m_waypoints[m_currentIndex]);
    }
}

void NavHelper::RefreshCTM()
{
    if (m_currentIndex >= m_waypoints.size())
        return;

    if (m_humanize) {
        // Raw lookahead WITHOUT noise or navmesh snap.
        // Avoids jitter/circling from time-varying noise + snap oscillation.
        auto player = game::GetLocalPlayer();
        if (!player) return;
        game::Vec3 myPos = player->GetPosition();
        game::Vec3 target = MovementSynth::GetLookaheadPoint(
            m_waypoints, m_currentIndex, myPos, kLookaheadDist);
        game::movement::ClickToMove(target);
    } else {
        game::movement::ClickToMove(m_waypoints[m_currentIndex]);
    }
}

std::vector<game::Vec3> NavHelper::GetRemainingPath() const
{
    std::vector<game::Vec3> remaining;

    auto player = game::GetLocalPlayer();
    if (player)
        remaining.push_back(player->GetPosition());

    for (size_t i = m_currentIndex; i < m_waypoints.size(); ++i)
        remaining.push_back(m_waypoints[i]);

    return remaining;
}

void NavHelper::ReplaceRemainingPath(const std::vector<game::Vec3>& newPath)
{
    // Remove everything from m_currentIndex onward
    if (m_currentIndex < m_waypoints.size())
        m_waypoints.erase(m_waypoints.begin() + m_currentIndex, m_waypoints.end());

    // Append new path
    m_waypoints.insert(m_waypoints.end(), newPath.begin(), newPath.end());

    // Clamp currentIndex to valid range (detour may be empty or shorter than original)
    if (m_currentIndex >= m_waypoints.size() && !m_waypoints.empty())
        m_currentIndex = m_waypoints.size() - 1;

    LOG(INFO) << "[NavHelper] Replaced remaining path: now " << m_waypoints.size()
              << " total waypoints, current index " << m_currentIndex;
}

const BlockingThreat* NavHelper::GetWeakestBlockingThreat() const
{
    return ThreatScanner::GetWeakestBlockingThreat(m_blockingThreats);
}

bool NavHelper::ShouldReroute(uint64_t now) const
{
    // Cooldown: minimum 5s between reroutes
    if (now - m_reroute.lastRerouteTime < RerouteState::kMinInterval)
        return false;

    // Rate limit: max 5 reroutes per 10s window
    if (now - m_reroute.windowStart < RerouteState::kWindowMs) {
        if (m_reroute.rerouteCount >= RerouteState::kMaxPerWindow) {
            LOG(WARNING) << "[NavHelper] Reroute rate limit hit (" << m_reroute.rerouteCount
                         << " in " << RerouteState::kWindowMs / 1000 << "s), using current path";
            return false;
        }
    }

    return true;
}

void NavHelper::CheckThreats()
{
    auto player = game::GetLocalPlayer();
    if (!player) return;

    game::Vec3 myPos = player->GetPosition();

    auto dangers = CollectDangerZones();
    if (dangers.empty()) {
        m_blockingThreats.clear();
        m_detourWaypoints.clear();
        m_currentPathThroughDanger = false;
        m_currentDangerPolyCount = 0;
        return;
    }

    if (m_waypoints.empty()) return;
    const game::Vec3& finalTarget = m_waypoints.back();

    auto result = nav::Pathfinder::Instance().FindPathAvoiding(myPos, finalTarget, dangers);

    if (!result.success || result.waypoints.empty())
        return;

    // --- Hysteresis: only accept new path if >15% better ---
    // Count danger polys in new path by checking straight path against threats
    int candidateDanger = 0;
    if (result.throughDanger) {
        // Rough estimate: count segments that intersect any danger zone
        for (size_t i = 0; i + 1 < result.waypoints.size(); ++i) {
            for (const auto& dz : dangers) {
                game::Vec3 dzPos = { dz.x, dz.y, dz.z };
                float buffered = dz.radius * 1.15f + 3.0f;
                if (ThreatScanner::SegmentIntersectsCircle2D(
                        result.waypoints[i], result.waypoints[i + 1], dzPos, buffered)) {
                    ++candidateDanger;
                    break;  // count segment once
                }
            }
        }
    }

    // Reject if switching from safe to dangerous
    if (m_currentDangerPolyCount == 0 && candidateDanger > 0)
        return;

    // Hysteresis check: if current path has danger, new path must be significantly better
    if (m_currentDangerPolyCount > 0 && candidateDanger > 0) {
        float improvement = 1.0f - static_cast<float>(candidateDanger) /
                                    static_cast<float>(m_currentDangerPolyCount);
        if (improvement < RerouteState::kHysteresis) {
            // Not enough improvement — keep current path
            return;
        }
    }

    // If both paths are safe (0 danger), only reroute if candidate is shorter
    if (m_currentDangerPolyCount == 0 && candidateDanger == 0) {
        // Compute rough path lengths
        float currentLen = 0.f;
        for (size_t i = m_currentIndex; i + 1 < m_waypoints.size(); ++i)
            currentLen += m_waypoints[i].Distance2D(m_waypoints[i + 1]);
        float candidateLen = 0.f;
        for (size_t i = 0; i + 1 < result.waypoints.size(); ++i)
            candidateLen += result.waypoints[i].Distance2D(result.waypoints[i + 1]);
        // Only reroute if >15% shorter
        if (candidateLen >= currentLen * (1.0f - RerouteState::kHysteresis))
            return;
    }

    // --- Accept new path ---
    m_detourWaypoints.clear();
    for (const auto& wp : result.waypoints) {
        bool isOriginal = false;
        for (size_t i = m_currentIndex; i < m_waypoints.size(); ++i) {
            if (wp.Distance2D(m_waypoints[i]) < 0.5f) {
                isOriginal = true;
                break;
            }
        }
        if (!isOriginal)
            m_detourWaypoints.push_back(wp);
    }

    std::vector<game::Vec3> newRemaining;
    for (size_t i = 0; i < result.waypoints.size(); ++i) {
        if (i == 0 && myPos.Distance2D(result.waypoints[0]) < 2.5f)
            continue;
        newRemaining.push_back(result.waypoints[i]);
    }
    ReplaceRemainingPath(newRemaining);
    IssueCTMToCurrentWP();

    // Update reroute tracking
    uint64_t now = GetTickCount64();
    if (now - m_reroute.windowStart >= RerouteState::kWindowMs) {
        m_reroute.rerouteCount = 0;
        m_reroute.windowStart = now;
    }
    m_reroute.rerouteCount++;
    m_reroute.lastRerouteTime = now;

    // Update danger state
    m_currentPathThroughDanger = result.throughDanger;
    m_currentDangerPolyCount = candidateDanger;
    m_blockingThreats.clear();

    LOG(INFO) << "[NavHelper] Rerouted around " << dangers.size()
              << " threats (" << m_detourWaypoints.size() << " detour WPs, "
              << candidateDanger << " danger segs)"
              << (result.throughDanger ? " [through danger]" : " [safe]")
              << " reroute#" << m_reroute.rerouteCount;

    // Enter recovery wait if path still goes through danger
    if (result.throughDanger && m_recoveryState == RecoveryState::None) {
        m_recoveryState = RecoveryState::Waiting;
        m_waitStartTime = GetTickCount64();
        m_lastWaitRecheck = 0;
        game::movement::StopCTM();
        LOG(INFO) << "[NavHelper] Entering recovery: waiting up to "
                  << kRecoveryWaitMs / 1000 << "s for mobs to move...";
    }
}

void NavHelper::TickRecovery()
{
    uint64_t now = GetTickCount64();
    uint64_t elapsed = now - m_waitStartTime;

    // Re-check every 2s during wait
    if (elapsed - m_lastWaitRecheck >= kRecoveryRecheckMs) {
        m_lastWaitRecheck = elapsed;

        auto dangers = CollectDangerZones();
        if (dangers.empty() || m_waypoints.empty()) {
            // Danger cleared!
            m_recoveryState = RecoveryState::None;
            m_currentPathThroughDanger = false;
            m_currentDangerPolyCount = 0;
            IssueCTMToCurrentWP();
            LOG(INFO) << "[NavHelper] Recovery: danger cleared, resuming";
            return;
        }

        auto player = game::GetLocalPlayer();
        if (!player) return;
        game::Vec3 myPos = player->GetPosition();
        const game::Vec3& finalTarget = m_waypoints.back();

        auto result = nav::Pathfinder::Instance().FindPathAvoiding(myPos, finalTarget, dangers);
        if (result.success && !result.throughDanger) {
            // Found a safe path — use it and resume
            m_recoveryState = RecoveryState::None;
            m_currentPathThroughDanger = false;
            m_currentDangerPolyCount = 0;

            std::vector<game::Vec3> newRemaining;
            for (size_t i = 0; i < result.waypoints.size(); ++i) {
                if (i == 0 && myPos.Distance2D(result.waypoints[0]) < 2.5f)
                    continue;
                newRemaining.push_back(result.waypoints[i]);
            }
            ReplaceRemainingPath(newRemaining);
            IssueCTMToCurrentWP();
            LOG(INFO) << "[NavHelper] Recovery: safe path found after "
                      << elapsed / 1000 << "s, resuming";
            return;
        }
    }

    // Wait expired — evaluate blocking threats and transition to Blocked
    if (elapsed >= kRecoveryWaitMs) {
        LOG(INFO) << "[NavHelper] Recovery: wait expired after "
                  << kRecoveryWaitMs / 1000 << "s, evaluating threats";

        // Build blocking threats from current path for caller to evaluate
        auto remaining = GetRemainingPath();
        m_blockingThreats = ThreatScanner::GetBlockingThreats(remaining, 0);

        m_recoveryState = RecoveryState::Blocked;
        m_status = Status::Blocked;
    }
}

} // namespace bot
