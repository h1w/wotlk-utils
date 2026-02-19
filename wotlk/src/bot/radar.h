#pragma once
// =============================================================================
// RadarData -- scans ObjectManager periodically and caches entries for the
// radar widget to render. Separates data collection from ImGui rendering.
// =============================================================================

#include "../game/types.h"
#include "../game/local_player.h"

#include <vector>
#include <string>
#include <cstdint>

namespace bot {

struct RadarEntry {
    game::Vec3       position;
    game::GUID       guid       = 0;
    std::string      name;
    int              level      = 0;
    float            healthPct  = 0.f;  // 0..100
    game::ObjectType objType    = game::ObjectType::Object;
    game::UnitReaction reaction = game::UnitReaction::Neutral;
    bool             isDead     = false;
    bool             isInCombat = false;
    bool             isPlayer   = false;
    float            aggroRadius = 0.f; // only for hostile NPCs (includes margin)
    float            distToPlayer = 0.f;// 2D distance
    float            facing     = 0.f;  // radians
};

class RadarData {
public:
    void Update(const game::LocalPlayer& player);

    const std::vector<RadarEntry>& GetEntries() const { return m_entries; }

    int hostileCount  = 0;
    int friendlyCount = 0;
    int neutralCount  = 0;
    int playerCount   = 0;
    int objectCount   = 0;

private:
    std::vector<RadarEntry> m_entries;
    uint64_t m_lastUpdateTick = 0;

    static constexpr uint32_t kUpdateIntervalMs = 200;
};

} // namespace bot
