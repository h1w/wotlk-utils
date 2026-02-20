#include "follow_route.h"
#include "attack.h"
#include "loot.h"
#include "sequence.h"
#include "../action_queue.h"
#include "../threat_scanner.h"
#include "../radar.h"
#include "../../game/game.h"
#include "../../game/movement.h"
#include "../../game/world.h"
#include "../../navigation/pathfinder.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdio>

#include <glog/logging.h>

namespace bot {

FollowRouteTool::FollowRouteTool(std::vector<game::Vec3> waypoints, bool loop,
                                 int forcedCombatCount)
    : m_waypoints(std::move(waypoints))
    , m_loop(loop)
    , m_forcedCombatCount(forcedCombatCount)
{
}

void FollowRouteTool::Start()
{
    if (!game::world::IsInGame() || m_waypoints.empty()) {
        m_status = ToolStatus::Failed;
        return;
    }

    m_currentIndex = 0;
    m_startTick = GetTickCount64();
    m_lastStuckCheckTick = m_startTick;
    m_lastCTMTick = m_startTick;
    m_stuckCount = 0;
    m_currentDangerSegCount = 0;
    m_reroute = RerouteState{};
    m_inMicroPause = false;
    m_microPauseEnd = 0;
    m_synth.Reset();
    m_status = ToolStatus::Running;

    auto player = game::GetLocalPlayer();
    if (player)
        m_lastPosition = player->GetPosition();

    IssueCTMToCurrentWP();
}

void FollowRouteTool::Tick()
{
    if (m_status != ToolStatus::Running)
        return;

    uint64_t now = GetTickCount64();

    // Micro-pause: bot briefly stops to mimic human behavior
    if (m_inMicroPause) {
        if (now >= m_microPauseEnd) {
            m_inMicroPause = false;
            IssueCTMToCurrentWP();
        }
        return;
    }

    // Timeout
    if (now - m_startTick > kTimeoutMs) {
        game::movement::StopCTM();
        m_status = ToolStatus::Failed;
        return;
    }

    auto player = game::GetLocalPlayer();
    if (!player) {
        m_status = ToolStatus::Failed;
        return;
    }

    game::Vec3 myPos = player->GetPosition();
    const game::Vec3& targetWP = m_waypoints[m_currentIndex];
    float dist = myPos.Distance2D(targetWP);

    // Arrived at current waypoint?
    if (dist < kArrivalThreshold) {
        AdvanceToNext();
        return;
    }

    // CTM refresh — re-issue periodically so lookahead target tracks player movement.
    // Without this, CTM reaches the lookahead point and player stops until stuck detection.
    // Use raw lookahead WITHOUT noise/snap to avoid jitter-induced circling.
    if (now - m_lastCTMTick >= kCTMRefreshMs) {
        m_lastCTMTick = now;
        RefreshCTM();
    }

    // Micro-pause check
    if (m_synth.ShouldMicroPause()) {
        m_inMicroPause = true;
        m_microPauseEnd = now + m_synth.GetPauseDurationMs();
        game::movement::StopCTM();
        return;
    }

    // Threat checking — event-based with fallback timer
    if (now >= m_reroute.lastFallbackCheck + RerouteState::kFallbackMs) {
        m_reroute.lastFallbackCheck = now;
        if (ShouldReroute(now)) {
            CheckThreatsAndReroute();
            return;
        }
    }

    // Stuck detection (every 3s)
    if (now - m_lastStuckCheckTick > kStuckCheckMs) {
        float moved = myPos.Distance2D(m_lastPosition);
        m_lastPosition = myPos;
        m_lastStuckCheckTick = now;

        if (moved < kStuckThreshold) {
            m_stuckCount++;
            if (m_stuckCount > kMaxStuckRetries) {
                game::movement::StopCTM();
                m_status = ToolStatus::Failed;
                return;
            }
            // Try to unstick: jump + re-issue CTM
            game::movement::Jump();
            IssueCTMToCurrentWP();
        } else {
            m_stuckCount = 0;
        }
    }
}

void FollowRouteTool::Abort()
{
    if (m_status == ToolStatus::Running || m_status == ToolStatus::Pending) {
        game::movement::StopCTM();
        game::movement::StopMoving();
        m_status = ToolStatus::Cancelled;
    }
}

void FollowRouteTool::AdvanceToNext()
{
    m_currentIndex++;

    if (m_currentIndex >= m_waypoints.size()) {
        if (m_loop) {
            m_currentIndex = 0;
        } else {
            game::movement::StopCTM();
            m_status = ToolStatus::Completed;
            return;
        }
    }

    m_stuckCount = 0;
    m_lastCTMTick = GetTickCount64();
    IssueCTMToCurrentWP();
}

void FollowRouteTool::IssueCTMToCurrentWP()
{
    if (m_currentIndex >= m_waypoints.size())
        return;

    auto player = game::GetLocalPlayer();
    game::Vec3 myPos = player ? player->GetPosition() : m_waypoints[m_currentIndex];
    game::Vec3 target = MovementSynth::GetLookaheadPoint(
        m_waypoints, m_currentIndex, myPos, kLookaheadDist);
    target = m_synth.AddNoise(target);
    // Snap noisy target back onto navmesh
    auto [snapped, ok] = MovementSynth::SnapToNavmesh(target);
    if (ok) target = snapped;
    game::movement::ClickToMove(target);
}

void FollowRouteTool::RefreshCTM()
{
    if (m_currentIndex >= m_waypoints.size())
        return;

    auto player = game::GetLocalPlayer();
    if (!player) return;
    game::Vec3 myPos = player->GetPosition();

    // Raw lookahead WITHOUT noise or navmesh snap.
    // Avoids jitter/circling from time-varying noise + snap oscillation.
    game::Vec3 target = MovementSynth::GetLookaheadPoint(
        m_waypoints, m_currentIndex, myPos, kLookaheadDist);
    game::movement::ClickToMove(target);
}

bool FollowRouteTool::ShouldReroute(uint64_t now) const
{
    if (now - m_reroute.lastRerouteTime < RerouteState::kMinInterval)
        return false;
    if (now - m_reroute.windowStart < RerouteState::kWindowMs) {
        if (m_reroute.rerouteCount >= RerouteState::kMaxPerWindow) {
            LOG(WARNING) << "[FollowRoute] Reroute rate limit hit, using current path";
            return false;
        }
    }
    return true;
}

void FollowRouteTool::CheckThreatsAndReroute()
{
    auto player = game::GetLocalPlayer();
    if (!player) return;

    game::Vec3 myPos = player->GetPosition();
    if (m_currentIndex >= m_waypoints.size()) return;

    // Build danger zones from hostile NPCs
    auto& radar = bot::RadarData::Instance();
    const auto& entries = radar.GetEntries();

    static constexpr float kMaxDangerDistance = 150.0f;

    std::vector<nav::DangerZone> dangers;
    for (const auto& e : entries) {
        if (e.isPlayer || e.isDead || e.isInCombat) continue;
        if (e.aggroRadiusNav <= 0.0f) continue;
        if (e.distToPlayer > kMaxDangerDistance) continue;
        if (e.reaction != game::UnitReaction::Hostile &&
            e.reaction != game::UnitReaction::Unfriendly)
            continue;
        dangers.push_back({ e.position.x, e.position.y, e.position.z, e.aggroRadiusNav });
    }

    if (dangers.empty()) {
        m_detourWaypoints.clear();
        m_currentDangerSegCount = 0;
        return;
    }

    // Check if current path intersects any threats
    std::vector<game::Vec3> remaining;
    remaining.push_back(myPos);
    for (size_t i = m_currentIndex; i < m_waypoints.size(); ++i)
        remaining.push_back(m_waypoints[i]);

    if (remaining.size() < 2) return;

    auto threats = ThreatScanner::GetBlockingThreats(remaining, 0);
    if (threats.empty()) {
        m_detourWaypoints.clear();
        m_currentDangerSegCount = 0;
        return;
    }

    // Only reroute if at least one threat is imminent (mob itself within 60yd).
    // Far-ahead threats may resolve by the time we get there (mobs move, patrol, etc.).
    static constexpr float kImminentThreatDist = 60.0f;
    bool anyImminent = false;
    for (const auto& t : threats) {
        if (t.entry.distToPlayer < kImminentThreatDist) {
            anyImminent = true;
            break;
        }
    }
    if (!anyImminent)
        return;

    // Reroute target: final destination (gives A* maximum room to find alternatives)
    size_t targetRouteIdx = m_waypoints.size() - 1;
    const game::Vec3& targetWP = m_waypoints[targetRouteIdx];

    auto result = nav::Pathfinder::Instance().FindPathAvoiding(myPos, targetWP, dangers);

    if (!result.success || result.waypoints.empty())
        return;

    // --- Hysteresis: count danger segments in candidate path ---
    int candidateDanger = 0;
    if (result.throughDanger) {
        for (size_t i = 0; i + 1 < result.waypoints.size(); ++i) {
            for (const auto& dz : dangers) {
                game::Vec3 dzPos = { dz.x, dz.y, dz.z };
                float buffered = dz.radius * 1.15f + 3.0f;
                if (ThreatScanner::SegmentIntersectsCircle2D(
                        result.waypoints[i], result.waypoints[i + 1], dzPos, buffered)) {
                    ++candidateDanger;
                    break;
                }
            }
        }
    }

    // Reject if switching from safe to dangerous
    if (m_currentDangerSegCount == 0 && candidateDanger > 0)
        return;

    // Only accept if >15% better (danger-to-danger)
    if (m_currentDangerSegCount > 0 && candidateDanger > 0) {
        float improvement = 1.0f - static_cast<float>(candidateDanger) /
                                    static_cast<float>(m_currentDangerSegCount);
        if (improvement < RerouteState::kHysteresis)
            return;
    }

    // Both safe: only accept if significantly shorter (prevents oscillation between
    // two equally-safe paths that go in different directions)
    if (m_currentDangerSegCount == 0 && candidateDanger == 0) {
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
        for (size_t i = m_currentIndex; i <= targetRouteIdx; ++i) {
            if (wp.Distance2D(m_waypoints[i]) < 0.5f) {
                isOriginal = true;
                break;
            }
        }
        if (!isOriginal)
            m_detourWaypoints.push_back(wp);
    }

    std::vector<game::Vec3> newWaypoints(
        m_waypoints.begin(), m_waypoints.begin() + m_currentIndex);
    for (size_t i = 0; i < result.waypoints.size(); ++i) {
        if (i == 0 && myPos.Distance2D(result.waypoints[0]) < 2.5f)
            continue;
        newWaypoints.push_back(result.waypoints[i]);
    }
    for (size_t i = targetRouteIdx + 1; i < m_waypoints.size(); ++i)
        newWaypoints.push_back(m_waypoints[i]);

    m_waypoints = std::move(newWaypoints);
    IssueCTMToCurrentWP();

    // Update reroute tracking
    uint64_t now = GetTickCount64();
    if (now - m_reroute.windowStart >= RerouteState::kWindowMs) {
        m_reroute.rerouteCount = 0;
        m_reroute.windowStart = now;
    }
    m_reroute.rerouteCount++;
    m_reroute.lastRerouteTime = now;
    m_currentDangerSegCount = candidateDanger;

    LOG(INFO) << "[FollowRoute] Rerouted around " << threats.size()
              << " threats to wp[" << targetRouteIdx << "] ("
              << m_detourWaypoints.size() << " detour WPs, "
              << candidateDanger << " danger segs)"
              << (result.throughDanger ? " [through danger]" : " [safe]")
              << " reroute#" << m_reroute.rerouteCount;
}

void FollowRouteTool::HandleBlockedPath(const std::vector<BlockingThreat>& threats)
{
    if (m_forcedCombatCount >= kMaxForcedCombats) {
        LOG(WARNING) << "[FollowRoute] Max forced combats (" << kMaxForcedCombats
                     << ") reached, failing";
        game::movement::StopCTM();
        m_status = ToolStatus::Failed;
        return;
    }

    auto player = game::GetLocalPlayer();
    if (!player) {
        m_status = ToolStatus::Failed;
        return;
    }

    int playerLevel = static_cast<int>(player->GetLevel());

    const BlockingThreat* weakest = ThreatScanner::GetWeakestBlockingThreat(threats);
    bool anyStrong = false;
    for (const auto& t : threats) {
        if (t.entry.level >= playerLevel + 3) {
            anyStrong = true;
            break;
        }
    }

    if (anyStrong || !weakest) {
        // Strong mobs — run through, don't fight
        LOG(INFO) << "[FollowRoute] Strong mobs blocking (" << threats.size()
                  << " threats), running through danger zone";
        IssueCTMToCurrentWP();
        return;
    }

    // Weak mob — attack, loot, resume
    LOG(INFO) << "[FollowRoute] Forced combat #" << (m_forcedCombatCount + 1)
              << " vs " << weakest->entry.name
              << " (Lv" << weakest->entry.level
              << ", player Lv" << playerLevel << " — weak)";

    m_inForcedCombat = true;
    game::movement::StopCTM();

    std::vector<game::Vec3> remainingWPs;
    for (size_t i = m_currentIndex; i < m_waypoints.size(); ++i)
        remainingWPs.push_back(m_waypoints[i]);

    std::vector<ToolPtr> steps;
    steps.push_back(std::make_unique<AttackTool>(weakest->entry.guid));
    steps.push_back(std::make_unique<LootTool>(weakest->entry.guid));
    steps.push_back(std::make_unique<FollowRouteTool>(
        std::move(remainingWPs), m_loop, m_forcedCombatCount + 1));

    auto seq = std::make_unique<SequenceTool>(std::move(steps));
    ActionQueue::Instance().Interrupt(std::move(seq));
}

float FollowRouteTool::GetDistanceToCurrentWP() const
{
    auto player = game::GetLocalPlayer();
    if (!player || m_currentIndex >= m_waypoints.size())
        return 0.0f;
    return player->GetPosition().Distance2D(m_waypoints[m_currentIndex]);
}

float FollowRouteTool::GetProgress() const
{
    if (m_waypoints.empty()) return 1.0f;
    return static_cast<float>(m_currentIndex) / static_cast<float>(m_waypoints.size());
}

std::string FollowRouteTool::Describe() const
{
    char buf[128];
    snprintf(buf, sizeof(buf), "Follow Route [%zu/%zu]%s%s",
             m_currentIndex + 1, m_waypoints.size(),
             m_loop ? " (loop)" : "",
             m_forcedCombatCount > 0 ? " [combat resume]" : "");
    return buf;
}

} // namespace bot
