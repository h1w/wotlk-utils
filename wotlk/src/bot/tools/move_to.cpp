#include "move_to.h"
#include "attack.h"
#include "loot.h"
#include "sequence.h"
#include "../action_queue.h"
#include "strategic_nav.h"
#include "../../game/game.h"
#include "../../game/movement.h"
#include "../../game/world.h"
#include "../../navigation/nav_mesh.h"
#include "../../navigation/world_graph.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdio>

#include <glog/logging.h>

namespace bot {

MoveToTool::MoveToTool(const game::Vec3& target, int forcedCombatCount)
    : m_target(target)
    , m_forcedCombatCount(forcedCombatCount)
{
}

ToolPtr MoveToTool::CreateSmart(const game::Vec3& target) {
    auto& graph = nav::WorldGraph::Instance();
    if (!graph.IsLoaded())
        return std::make_unique<MoveToTool>(target);

    auto player = game::GetLocalPlayer();
    if (!player || !game::world::IsInGame())
        return std::make_unique<MoveToTool>(target);

    game::Vec3 pos = player->GetPosition();
    float dist = pos.DistanceTo(target);

    // Only use strategic navigation for long distances where direct navmesh
    // pathfinding may fail due to tile coverage
    static constexpr float kStrategicThreshold = 2000.0f;
    if (dist < kStrategicThreshold)
        return std::make_unique<MoveToTool>(target);

    // Find nearest graph nodes to start and end
    uint32_t mapId = game::world::GetMapId();
    const auto* startNode = graph.FindNearestNode(mapId, pos.x, pos.y);
    const auto* endNode   = graph.FindNearestNode(mapId, target.x, target.y);

    if (!startNode || !endNode)
        return std::make_unique<MoveToTool>(target);

    // Plan route on world graph
    auto route = graph.PlanRoute(startNode->id, endNode->id);
    if (route.empty())
        return std::make_unique<MoveToTool>(target);

    LOG(INFO) << "[MoveToTool] Long distance (" << dist << "yd), using strategic nav ("
              << route.size() << " segments)";

    return std::make_unique<StrategicNavTool>(std::move(route));
}

void MoveToTool::Start()
{
    if (!game::world::IsInGame()) {
        m_status = ToolStatus::Failed;
        return;
    }

    auto player = game::GetLocalPlayer();
    if (!player) {
        m_status = ToolStatus::Failed;
        return;
    }

    m_status = ToolStatus::Running;

    game::Vec3 startPos = player->GetPosition();
    float totalDist = startPos.DistanceTo(m_target);

    // Pre-load corridor tiles for long walks
    if (totalDist > kCorridorMinDist) {
        m_corridor.PlanCorridor(startPos, m_target);
        int loaded = m_corridor.LoadCorridor();
        if (loaded > 0)
            LOG(INFO) << "[MoveToTool] Pre-loaded " << loaded << " corridor tiles for "
                      << totalDist << "yd walk";
    }

    // Try navmesh pathfinding first
    m_nav.SetHumanizeEnabled(true);
    m_nav.SetAvoidanceEnabled(true);   // MUST be before StartNavTo so initial path avoids dangers
    if (m_nav.StartNavTo(m_target)) {
        m_useNav = true;
        LOG(INFO) << "[MoveToTool] Using navmesh path (avoidance enabled, combat count="
                  << m_forcedCombatCount << ")";
    } else {
        // Fallback to direct CTM
        m_useNav = false;
        LOG(INFO) << "[MoveToTool] Navmesh unavailable, using direct CTM";
        StartDirectCTM();
    }
}

void MoveToTool::Tick()
{
    if (m_status != ToolStatus::Running)
        return;

    auto player = game::GetLocalPlayer();
    if (!player) {
        m_status = ToolStatus::Failed;
        return;
    }

    game::Vec3 pos = player->GetPosition();
    float dist = pos.DistanceTo(m_target);

    // Arrived? (check regardless of nav mode)
    if (dist <= kArrivalDist) {
        if (m_useNav)
            m_nav.Stop();
        else
            game::movement::StopCTM();
        m_corridor.Clear();
        m_status = ToolStatus::Completed;
        return;
    }

    // Unload corridor tiles behind player during long walks
    if (m_corridor.HasCorridor())
        m_corridor.UnloadBehind(pos.x, pos.y);

    if (m_useNav) {
        auto navStatus = m_nav.Tick();
        switch (navStatus) {
        case NavHelper::Status::Arrived:
            game::movement::StopCTM();
            if (dist <= kArrivalDist) {
                m_status = ToolStatus::Completed;
            } else {
                m_useNav = false;
                StartDirectCTM();
            }
            break;
        case NavHelper::Status::Failed:
            LOG(WARNING) << "[MoveToTool] Nav failed, falling back to direct CTM";
            m_useNav = false;
            StartDirectCTM();
            break;
        case NavHelper::Status::Blocked:
            HandleBlockedPath();
            return; // 'this' may be destroyed by Interrupt() — don't access members
        case NavHelper::Status::Moving:
            break;
        default:
            break;
        }
    } else {
        // Direct CTM mode — stuck detection
        float moved = pos.DistanceTo(m_lastPos);
        uint64_t now = GetTickCount64();

        if (moved >= kStuckThreshold) {
            m_lastPos = pos;
            m_lastProgressTick = now;
        } else if (now - m_lastProgressTick > kStuckTimeoutMs) {
            m_retries++;
            if (m_retries > kMaxRetries) {
                m_status = ToolStatus::Failed;
                return;
            }
            m_lastPos = pos;
            m_lastProgressTick = now;
            game::movement::Jump();
            game::movement::ClickToMove(m_target);
        }
    }
}

void MoveToTool::Abort()
{
    if (m_status == ToolStatus::Running || m_status == ToolStatus::Pending) {
        if (m_useNav)
            m_nav.Stop();
        else
            game::movement::StopCTM();
        game::movement::StopMoving();
        m_corridor.Clear();
        m_status = ToolStatus::Cancelled;
    }
}

std::string MoveToTool::Describe() const
{
    char buf[128];
    snprintf(buf, sizeof(buf), "MoveTo (%.1f, %.1f, %.1f)%s%s",
             m_target.x, m_target.y, m_target.z,
             m_useNav ? " [nav]" : " [direct]",
             m_forcedCombatCount > 0 ? " [combat resume]" : "");
    return buf;
}

float MoveToTool::GetDistanceRemaining() const
{
    auto player = game::GetLocalPlayer();
    if (!player) return 0.0f;
    return player->GetPosition().DistanceTo(m_target);
}

void MoveToTool::StartDirectCTM()
{
    auto player = game::GetLocalPlayer();
    if (player) {
        m_lastPos = player->GetPosition();
        m_lastProgressTick = GetTickCount64();
    }
    m_retries = 0;
    game::movement::ClickToMove(m_target);
}

void MoveToTool::HandleBlockedPath()
{
    if (m_forcedCombatCount >= kMaxForcedCombats) {
        LOG(WARNING) << "[MoveToTool] Max forced combats (" << kMaxForcedCombats
                     << ") reached, failing";
        m_status = ToolStatus::Failed;
        return;
    }

    auto player = game::GetLocalPlayer();
    if (!player) {
        m_status = ToolStatus::Failed;
        return;
    }

    const auto& threats = m_nav.GetBlockingThreats();
    if (threats.empty()) {
        LOG(WARNING) << "[MoveToTool] Blocked but no threat info, failing";
        m_status = ToolStatus::Failed;
        return;
    }

    int playerLevel = static_cast<int>(player->GetLevel());

    // Evaluate strength: weak = level < player + 3, strong otherwise
    const BlockingThreat* weakest = m_nav.GetWeakestBlockingThreat();
    bool anyStrong = false;
    for (const auto& t : threats) {
        if (t.entry.level >= playerLevel + 3) {
            anyStrong = true;
            break;
        }
    }

    if (anyStrong || !weakest) {
        // Strong mobs or no target — run through on current path (don't fight)
        LOG(INFO) << "[MoveToTool] Strong mobs blocking (" << threats.size()
                  << " threats), running through danger zone";
        // Resume movement on the throughDanger path — don't stop, don't fight
        m_nav.SetAvoidanceEnabled(false);  // temporarily disable to avoid re-triggering recovery
        if (m_nav.StartNavTo(m_target)) {
            m_nav.SetAvoidanceEnabled(true);
        } else {
            // Can't even nav — direct CTM
            m_useNav = false;
            StartDirectCTM();
        }
        return;
    }

    // Weak mob — attack, loot, resume
    LOG(INFO) << "[MoveToTool] Forced combat #" << (m_forcedCombatCount + 1)
              << " vs " << weakest->entry.name
              << " (Lv" << weakest->entry.level
              << ", player Lv" << playerLevel << " — weak)";

    m_inForcedCombat = true;

    // Build: Attack -> Loot -> MoveToTool(same target, count+1)
    std::vector<ToolPtr> steps;
    steps.push_back(std::make_unique<AttackTool>(weakest->entry.guid));
    steps.push_back(std::make_unique<LootTool>(weakest->entry.guid));
    steps.push_back(std::make_unique<MoveToTool>(m_target, m_forcedCombatCount + 1));

    auto seq = std::make_unique<SequenceTool>(std::move(steps));
    ActionQueue::Instance().Interrupt(std::move(seq));
}

} // namespace bot
