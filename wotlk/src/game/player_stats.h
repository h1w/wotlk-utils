#pragma once
#include "types.h"
#include <string>
#include <cstdint>

namespace game {

struct PlayerStats {
    // --- Identity ---
    struct Info {
        std::string name;
        std::string realm;
        uint8_t race = 0;
        uint8_t classId = 0;
        uint32_t level = 0;
    } info;

    // --- Resources ---
    struct Resources {
        uint32_t health = 0, maxHealth = 0;
        uint32_t power = 0, maxPower = 0;
        const char* powerLabel = "Mana"; // "Mana"/"Rage"/"Energy"/"Runic Power"
        uint32_t xp = 0, nextLevelXp = 0;
        uint32_t coinage = 0; // in copper
    } resources;

    // --- Location ---
    struct Location {
        std::string zone;
        std::string subZone;
        Vec3 position;
    } location;

    // --- Base Stats ---
    struct Base {
        uint32_t strength = 0, agility = 0, stamina = 0, intellect = 0, spirit = 0;
        uint32_t armor = 0;
        uint32_t holyRes = 0, fireRes = 0, natureRes = 0, frostRes = 0, shadowRes = 0, arcaneRes = 0;
        uint32_t baseMana = 0, baseHealth = 0;
    } base;

    // --- Melee ---
    struct Melee {
        float minDamage = 0, maxDamage = 0;
        float minOffhandDmg = 0, maxOffhandDmg = 0;
        float speed = 0; // seconds (main hand)
        float offhandSpeed = 0;
        int32_t attackPower = 0; // base + mods
        float attackPowerMultiplier = 0;
        uint32_t hitRating = 0;
        float critChance = 0;
        uint32_t expertise = 0;
        uint32_t offhandExpertise = 0;
        uint32_t hasteRating = 0;
        uint32_t armorPenRating = 0;
    } melee;

    // --- Ranged ---
    struct Ranged {
        float minDamage = 0, maxDamage = 0;
        float speed = 0;
        int32_t attackPower = 0;
        float attackPowerMultiplier = 0;
        uint32_t hitRating = 0;
        float critChance = 0;
        uint32_t hasteRating = 0;
    } ranged;

    // --- Spell ---
    struct Spell {
        int32_t bonusDamage[7] = {}; // per school: physical,holy,fire,nature,frost,shadow,arcane
        int32_t maxBonusDamage = 0;  // max across non-physical schools
        int32_t bonusHealing = 0;
        uint32_t hitRating = 0;
        float critChance[7] = {};    // per school
        float maxCritChance = 0;     // max across non-physical schools
        uint32_t hasteRating = 0;
        float manaRegen5 = 0;        // per 5 sec (not casting)
        float manaRegen5Combat = 0;  // per 5 sec (while casting)
    } spell;

    // --- Defenses ---
    struct Defense {
        uint32_t armor = 0;
        uint16_t defenseSkill = 0;   // from PLAYER_SKILL_INFO
        uint32_t defenseRating = 0;  // combat rating from gear
        float dodge = 0, parry = 0, block = 0; // percentages
        uint32_t resilience = 0;
        uint32_t shieldBlock = 0;    // block value
    } defense;

    // --- PvP ---
    struct PvP {
        uint32_t honorCurrency = 0;
        uint32_t arenaCurrency = 0;
        uint32_t kills = 0;          // today's HK field
        uint32_t lifetimeHKs = 0;
    } pvp;
};

// Snapshot all stats from the local player. Call once per frame.
class LocalPlayer; // forward decl
PlayerStats GatherPlayerStats(const LocalPlayer& player);

} // namespace game
