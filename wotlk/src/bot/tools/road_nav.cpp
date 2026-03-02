#include "road_nav.h"
#include "../radar.h"
#include "../../game/game.h"
#include "../../game/world.h"
#include "../../navigation/road_graph.h"

#include <cstdio>

#include <glog/logging.h>

namespace bot {

RoadNavTool::RoadNavTool(const game::Vec3& start, const game::Vec3& end,
                         nav::RoadPlan plan)
    : m_start(start)
    , m_end(end)
    , m_plan(std::move(plan))
{
}

void RoadNavTool::Start() {
    if (!game::world::IsInGame()) {
        m_status = ToolStatus::Failed;
        return;
    }

    auto player = game::GetLocalPlayer();
    if (!player) {
        m_status = ToolStatus::Failed;
        return;
    }

    if (m_plan.roadPath.empty()) {
        m_status = ToolStatus::Failed;
        return;
    }

    m_status = ToolStatus::Running;

    LOG(INFO) << "[RoadNav] Starting road navigation: "
              << m_plan.roadPath.size() << " road nodes, "
              << m_plan.roadDist << " yd road distance";

    // Phase 1: Approach — walk to the entry road node
    game::Vec3 entryPos = GetRoadNodePos(0);
    float approachDist = player->GetPosition().DistanceTo(entryPos);

    if (approachDist < kRoadArrivalThreshold) {
        // Already near the entry node — skip approach phase
        m_phase = Phase::RoadFollow;
        m_roadIndex = 1; // skip first node (we're already there)
        if (m_roadIndex >= m_plan.roadPath.size()) {
            // Only one road node — go directly to departure
            m_phase = Phase::Departure;
            m_nav.SetHumanizeEnabled(true);
            m_nav.SetAvoidanceEnabled(true);
            m_nav.StartNavTo(m_end);
        } else {
            StartNextRoadSegment();
        }
    } else {
        m_phase = Phase::Approach;
        m_nav.SetHumanizeEnabled(true);
        m_nav.SetAvoidanceEnabled(true);
        if (!m_nav.StartNavTo(entryPos)) {
            // Can't pathfind to entry node — fall back to direct navmesh
            LOG(WARNING) << "[RoadNav] Can't reach road entry, falling back to navmesh";
            m_phase = Phase::Departure;
            m_nav.StartNavTo(m_end);
        }
    }
}

void RoadNavTool::Tick() {
    if (m_status != ToolStatus::Running)
        return;

    auto player = game::GetLocalPlayer();
    if (!player) {
        m_status = ToolStatus::Failed;
        return;
    }

    game::Vec3 pos = player->GetPosition();

    switch (m_phase) {
    case Phase::Approach: {
        auto navStatus = m_nav.Tick();
        if (navStatus == NavHelper::Status::Arrived) {
            // Reached road entry — start following road
            m_phase = Phase::RoadFollow;
            m_roadIndex = 1; // first road node was the approach target
            m_skippedConsecutive = 0;
            if (m_roadIndex >= m_plan.roadPath.size()) {
                m_phase = Phase::Departure;
                m_nav.StartNavTo(m_end);
            } else {
                StartNextRoadSegment();
            }
        } else if (navStatus == NavHelper::Status::Failed ||
                   navStatus == NavHelper::Status::Blocked) {
            // Can't reach road entry — fallback to direct navmesh to final destination
            LOG(WARNING) << "[RoadNav] Approach failed, falling back to navmesh";
            m_phase = Phase::Departure;
            m_nav.StartNavTo(m_end);
        }
        break;
    }

    case Phase::RoadFollow: {
        auto navStatus = m_nav.Tick();

        // Check arrival by both NavHelper status and distance to road node
        game::Vec3 currentTarget = GetRoadNodePos(m_roadIndex);
        float distToTarget = pos.Distance2D(currentTarget);

        if (navStatus == NavHelper::Status::Arrived ||
            distToTarget < kRoadArrivalThreshold) {
            // Arrived at current road node — advance
            m_skippedConsecutive = 0;
            m_roadIndex++;
            if (m_roadIndex >= m_plan.roadPath.size()) {
                // All road nodes traversed — departure phase
                m_phase = Phase::Departure;
                m_nav.StartNavTo(m_end);
            } else {
                StartNextRoadSegment();
            }
        } else if (navStatus == NavHelper::Status::Failed ||
                   navStatus == NavHelper::Status::Blocked) {
            // Can't reach this road node — skip it
            SkipCurrentRoadNode();
        }
        break;
    }

    case Phase::Departure: {
        auto navStatus = m_nav.Tick();
        if (navStatus == NavHelper::Status::Arrived) {
            m_phase = Phase::Done;
            m_status = ToolStatus::Completed;
            LOG(INFO) << "[RoadNav] Road navigation completed";
        } else if (navStatus == NavHelper::Status::Failed ||
                   navStatus == NavHelper::Status::Blocked) {
            m_phase = Phase::Failed;
            m_status = ToolStatus::Failed;
            LOG(WARNING) << "[RoadNav] Departure failed (status="
                         << static_cast<int>(navStatus) << ")";
        }
        break;
    }

    case Phase::Done:
        m_status = ToolStatus::Completed;
        break;
    case Phase::Failed:
        m_status = ToolStatus::Failed;
        break;
    }
}

void RoadNavTool::Abort() {
    if (m_status == ToolStatus::Running || m_status == ToolStatus::Pending) {
        m_nav.Stop();
        m_status = ToolStatus::Cancelled;
    }
}

std::string RoadNavTool::Describe() const {
    const char* phaseStr = "?";
    switch (m_phase) {
    case Phase::Approach:   phaseStr = "approach"; break;
    case Phase::RoadFollow: phaseStr = "road"; break;
    case Phase::Departure:  phaseStr = "departure"; break;
    case Phase::Done:       phaseStr = "done"; break;
    case Phase::Failed:     phaseStr = "failed"; break;
    }

    char buf[128];
    snprintf(buf, sizeof(buf), "RoadNav [%s %zu/%zu] (%.0f yd road)",
             phaseStr,
             m_roadIndex, m_plan.roadPath.size(),
             m_plan.roadDist);
    return buf;
}

void RoadNavTool::StartNextRoadSegment() {
    // Iterative loop — avoids recursion between StartNextRoadSegment/SkipCurrentRoadNode
    while (m_roadIndex < m_plan.roadPath.size()) {
        game::Vec3 nodePos = GetRoadNodePos(m_roadIndex);

        // Check if this node is near a threat — skip if dangerous
        if (IsNodeNearThreat(nodePos)) {
            m_skippedConsecutive++;
            m_roadIndex++;
            if (m_skippedConsecutive > kMaxConsecutiveSkips) {
                LOG(WARNING) << "[RoadNav] " << m_skippedConsecutive
                             << " consecutive skips, abandoning road";
                m_phase = Phase::Departure;
                m_nav.StartNavTo(m_end);
                return;
            }
            LOG(INFO) << "[RoadNav] Skipping node (danger), trying next";
            continue;
        }

        m_nav.SetHumanizeEnabled(true);
        m_nav.SetAvoidanceEnabled(true);
        if (!m_nav.StartNavTo(nodePos)) {
            // Can't pathfind to this road node — skip
            m_skippedConsecutive++;
            m_roadIndex++;
            if (m_skippedConsecutive > kMaxConsecutiveSkips) {
                LOG(WARNING) << "[RoadNav] " << m_skippedConsecutive
                             << " consecutive skips, abandoning road";
                m_phase = Phase::Departure;
                m_nav.StartNavTo(m_end);
                return;
            }
            LOG(INFO) << "[RoadNav] Skipping node (unreachable), trying next";
            continue;
        }

        LOG_EVERY_N(INFO, 10) << "[RoadNav] Segment " << m_roadIndex
                               << "/" << m_plan.roadPath.size()
                               << " -> node " << m_plan.roadPath[m_roadIndex];
        return;
    }

    // Exhausted all road nodes — departure
    m_phase = Phase::Departure;
    m_nav.StartNavTo(m_end);
}

void RoadNavTool::SkipCurrentRoadNode() {
    // Called from Tick() when NavHelper fails/blocks mid-segment
    m_skippedConsecutive++;
    m_roadIndex++;

    if (m_skippedConsecutive > kMaxConsecutiveSkips) {
        LOG(WARNING) << "[RoadNav] " << m_skippedConsecutive
                     << " consecutive skips, abandoning road";
        m_phase = Phase::Departure;
        m_nav.StartNavTo(m_end);
        return;
    }

    if (m_roadIndex >= m_plan.roadPath.size()) {
        m_phase = Phase::Departure;
        m_nav.StartNavTo(m_end);
        return;
    }

    LOG(INFO) << "[RoadNav] Skipping node (danger/unreachable), trying next";
    StartNextRoadSegment();  // iterative inside — no recursion risk
}

bool RoadNavTool::IsNodeNearThreat(const game::Vec3& nodePos) const {
    const auto& entries = RadarData::Instance().GetEntries();

    for (const auto& e : entries) {
        // Only check hostile, alive, non-player NPCs
        if (e.reaction != game::UnitReaction::Hostile)
            continue;
        if (e.isDead || e.isPlayer)
            continue;

        float dist = nodePos.Distance2D(e.position);
        // Use buffered aggro radius (R*1.15+3.0) plus extra margin
        float dangerRadius = e.aggroRadiusBuffered + kThreatMargin;

        if (dist < dangerRadius)
            return true;
    }

    return false;
}

game::Vec3 RoadNavTool::GetRoadNodePos(size_t pathIndex) const {
    if (pathIndex >= m_plan.roadPath.size())
        return m_end;

    uint32_t nodeId = m_plan.roadPath[pathIndex];
    const auto* node = nav::RoadGraph::Instance().GetNode(nodeId);
    if (!node)
        return m_end;

    return { node->x, node->y, node->z };
}

} // namespace bot
