#include "radar.h"
#include "aggro.h"
#include "../game/game.h"

#define NOMINMAX
#include <Windows.h>

#include <algorithm>

namespace bot {

RadarData& RadarData::Instance()
{
    static RadarData s_instance;
    return s_instance;
}

void RadarData::Update(const game::LocalPlayer& player)
{
    uint64_t now = GetTickCount64();
    if (now - m_lastUpdateTick < kUpdateIntervalMs)
        return;
    m_lastUpdateTick = now;

    m_entries.clear();
    hostileCount = friendlyCount = neutralCount = playerCount = objectCount = 0;

    game::Vec3 myPos = player.GetPosition();
    game::GUID myGuid = player.GetGUID();
    int myLevel = static_cast<int>(player.GetLevel());

    // --- Units (NPCs + other players) ---
    auto units = game::GetAllUnits();
    for (auto& u : units) {
        if (u.GetGUID() == myGuid)
            continue;

        RadarEntry e;
        e.guid       = u.GetGUID();
        e.position   = u.GetPosition();
        e.name       = u.GetUnitName();
        e.level      = static_cast<int>(u.GetLevel());
        e.healthPct  = u.GetHealthPercent();
        e.objType    = u.GetType();
        e.isDead     = u.IsDead();
        e.isInCombat = u.InCombat();
        e.isPlayer   = u.IsPlayer();
        e.facing     = u.GetFacing();

        e.reaction   = u.GetReaction(player);

        // Aggro radius for hostile NPCs (not players, not dead)
        if (!e.isPlayer && !e.isDead &&
            (e.reaction == game::UnitReaction::Hostile ||
             e.reaction == game::UnitReaction::Unfriendly))
        {
            e.aggroRadiusRaw = CalcAggroRadius(e.level, myLevel);
            e.aggroRadiusBuffered = CalcAggroRadiusBuffered(e.level, myLevel);
            e.aggroRadiusNav = CalcAggroRadiusNav(e.level, myLevel);
        }

        e.distToPlayer = myPos.Distance2D(e.position);

        // Update counters
        if (e.isPlayer) {
            ++playerCount;
        } else if (e.reaction == game::UnitReaction::Hostile ||
                   e.reaction == game::UnitReaction::Unfriendly) {
            ++hostileCount;
        } else if (e.reaction == game::UnitReaction::Friendly ||
                   e.reaction == game::UnitReaction::Honored) {
            ++friendlyCount;
        } else {
            ++neutralCount;
        }

        m_entries.push_back(std::move(e));
    }

    // --- GameObjects ---
    auto objects = game::GetAllGameObjects();
    for (auto& obj : objects) {
        RadarEntry e;
        e.guid        = obj.GetGUID();
        e.position    = obj.GetPosition();
        e.name        = obj.GetName();
        e.objType     = game::ObjectType::GameObject;
        e.reaction    = game::UnitReaction::Neutral;
        e.distToPlayer = myPos.Distance2D(e.position);

        ++objectCount;
        m_entries.push_back(std::move(e));
    }

    // Sort by distance (far to near) so near entries draw on top
    std::sort(m_entries.begin(), m_entries.end(),
        [](const RadarEntry& a, const RadarEntry& b) {
            return a.distToPlayer > b.distToPlayer;
        });
}

} // namespace bot
