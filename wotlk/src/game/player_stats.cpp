#include "player_stats.h"
#include "local_player.h"
#include "world.h"
#include "mem.h"
#include "../offsets/offsets.h"

namespace game {

// Helper: read defense skill (ID=95) from PLAYER_SKILL_INFO array
static uint16_t ReadDefenseSkill(const LocalPlayer& p)
{
    namespace f = offsets::fields;
    for (int i = 0; i < 128; ++i) {
        uint32_t packed = p.GetDescU32(f::PLAYER_SKILL_INFO + i * 3);
        uint16_t skillId = static_cast<uint16_t>(packed & 0xFFFF);
        if (skillId == 95) {
            uint32_t valPacked = p.GetDescU32(f::PLAYER_SKILL_INFO + i * 3 + 1);
            return static_cast<uint16_t>(valPacked & 0xFFFF);
        }
        if (skillId == 0) break;
    }
    return 0;
}

// Helper: power type label
static const char* PowerLabel(uint8_t classId)
{
    switch (classId) {
    case 1:  return "Rage";
    case 4:  return "Energy";
    case 6:  return "Runic Power";
    default: return "Mana";
    }
}

// Helper: primary power type index (0=mana,1=rage,2=focus,3=energy,4=happiness,5=runes,6=runic)
static int PrimaryPowerIndex(uint8_t classId)
{
    switch (classId) {
    case 1:  return 1; // rage
    case 4:  return 3; // energy
    case 6:  return 5; // runic power (index 5 in the power array, but actually...)
    default: return 0; // mana
    }
}

PlayerStats GatherPlayerStats(const LocalPlayer& p)
{
    namespace f = offsets::fields;
    PlayerStats s;

    // --- Identity ---
    s.info.name    = p.GetPlayerName();
    s.info.realm   = world::GetRealmName();
    uint32_t bytes0 = p.GetDescU32(f::UNIT_FIELD_BYTES_0);
    s.info.race    = static_cast<uint8_t>(bytes0 & 0xFF);
    s.info.classId = static_cast<uint8_t>((bytes0 >> 8) & 0xFF);
    s.info.level   = p.GetDescU32(f::UNIT_LEVEL);

    // --- Resources ---
    s.resources.health    = p.GetDescU32(f::UNIT_HEALTH);
    s.resources.maxHealth = p.GetDescU32(f::UNIT_MAXHEALTH);
    int pwrIdx = PrimaryPowerIndex(s.info.classId);
    s.resources.power     = p.GetDescU32(f::UNIT_POWER1 + pwrIdx);
    s.resources.maxPower  = p.GetDescU32(f::UNIT_MAXPOWER1 + pwrIdx);
    s.resources.powerLabel = PowerLabel(s.info.classId);
    s.resources.xp          = p.GetDescU32(f::PLAYER_XP);
    s.resources.nextLevelXp = p.GetDescU32(f::PLAYER_NEXT_LEVEL_XP);
    s.resources.coinage     = p.GetDescU32(f::PLAYER_COINAGE);

    // --- Location ---
    s.location.zone    = world::GetZoneText();
    s.location.subZone = world::GetSubZoneText();
    s.location.position = p.GetPosition();

    // --- Base Stats ---
    s.base.strength  = p.GetDescU32(f::UNIT_STAT0);
    s.base.agility   = p.GetDescU32(f::UNIT_STAT1);
    s.base.stamina   = p.GetDescU32(f::UNIT_STAT2);
    s.base.intellect = p.GetDescU32(f::UNIT_STAT3);
    s.base.spirit    = p.GetDescU32(f::UNIT_STAT4);
    // Resistances: [0]=armor, [1]=holy, [2]=fire, [3]=nature, [4]=frost, [5]=shadow, [6]=arcane
    s.base.armor     = p.GetDescU32(f::UNIT_RESISTANCES + 0);
    s.base.holyRes   = p.GetDescU32(f::UNIT_RESISTANCES + 1);
    s.base.fireRes   = p.GetDescU32(f::UNIT_RESISTANCES + 2);
    s.base.natureRes = p.GetDescU32(f::UNIT_RESISTANCES + 3);
    s.base.frostRes  = p.GetDescU32(f::UNIT_RESISTANCES + 4);
    s.base.shadowRes = p.GetDescU32(f::UNIT_RESISTANCES + 5);
    s.base.arcaneRes = p.GetDescU32(f::UNIT_RESISTANCES + 6);
    s.base.baseMana   = p.GetDescU32(f::UNIT_BASE_MANA);
    s.base.baseHealth = p.GetDescU32(f::UNIT_BASE_HEALTH);

    // --- Melee ---
    s.melee.minDamage      = p.GetDescFloat(f::UNIT_MINDAMAGE);
    s.melee.maxDamage      = p.GetDescFloat(f::UNIT_MAXDAMAGE);
    s.melee.minOffhandDmg  = p.GetDescFloat(f::UNIT_MINOFFHANDDAMAGE);
    s.melee.maxOffhandDmg  = p.GetDescFloat(f::UNIT_MAXOFFHANDDAMAGE);
    s.melee.speed          = p.GetDescU32(f::UNIT_BASEATTACKTIME) / 1000.f;
    s.melee.offhandSpeed   = p.GetDescU32(f::UNIT_BASEATTACKTIME + 1) / 1000.f;
    int32_t ap    = static_cast<int32_t>(p.GetDescU32(f::UNIT_ATTACK_POWER));
    int32_t apMod = static_cast<int32_t>(p.GetDescU32(f::UNIT_ATTACK_POWER_MODS));
    s.melee.attackPower    = ap + apMod;
    s.melee.attackPowerMultiplier = p.GetDescFloat(f::UNIT_ATTACK_POWER_MULT);
    s.melee.hitRating      = p.GetDescU32(f::PLAYER_COMBAT_RATING + f::CR_HIT_MELEE);
    s.melee.critChance     = p.GetDescFloat(f::PLAYER_CRIT_PERCENTAGE);
    s.melee.expertise      = p.GetDescU32(f::PLAYER_EXPERTISE);
    s.melee.offhandExpertise = p.GetDescU32(f::PLAYER_OFFHAND_EXPERTISE);
    s.melee.hasteRating    = p.GetDescU32(f::PLAYER_COMBAT_RATING + f::CR_HASTE_MELEE);
    s.melee.armorPenRating = p.GetDescU32(f::PLAYER_COMBAT_RATING + f::CR_ARMOR_PEN);

    // --- Ranged ---
    s.ranged.minDamage     = p.GetDescFloat(f::UNIT_MINRANGEDDAMAGE);
    s.ranged.maxDamage     = p.GetDescFloat(f::UNIT_MAXRANGEDDAMAGE);
    s.ranged.speed         = p.GetDescU32(f::UNIT_RANGEDATTACKTIME) / 1000.f;
    int32_t rap    = static_cast<int32_t>(p.GetDescU32(f::UNIT_RANGED_ATTACK_POWER));
    int32_t rapMod = static_cast<int32_t>(p.GetDescU32(f::UNIT_RANGED_ATTACK_POWER_MODS));
    s.ranged.attackPower   = rap + rapMod;
    s.ranged.attackPowerMultiplier = p.GetDescFloat(f::UNIT_RANGED_ATTACK_POWER_MULT);
    s.ranged.hitRating     = p.GetDescU32(f::PLAYER_COMBAT_RATING + f::CR_HIT_RANGED);
    s.ranged.critChance    = p.GetDescFloat(f::PLAYER_RANGED_CRIT_PERCENTAGE);
    s.ranged.hasteRating   = p.GetDescU32(f::PLAYER_COMBAT_RATING + f::CR_HASTE_RANGED);

    // --- Spell ---
    s.spell.maxBonusDamage = 0;
    for (int i = 0; i < 7; ++i) {
        s.spell.bonusDamage[i] = static_cast<int32_t>(p.GetDescU32(f::PLAYER_MOD_DAMAGE_DONE_POS + i));
        if (i >= 1 && s.spell.bonusDamage[i] > s.spell.maxBonusDamage)
            s.spell.maxBonusDamage = s.spell.bonusDamage[i];
    }
    s.spell.bonusHealing = static_cast<int32_t>(p.GetDescU32(f::PLAYER_MOD_HEALING_DONE_POS));
    s.spell.hitRating    = p.GetDescU32(f::PLAYER_COMBAT_RATING + f::CR_HIT_SPELL);
    s.spell.maxCritChance = 0;
    for (int i = 0; i < 7; ++i) {
        s.spell.critChance[i] = p.GetDescFloat(f::PLAYER_SPELL_CRIT_PERCENTAGE + i);
        if (i >= 1 && s.spell.critChance[i] > s.spell.maxCritChance)
            s.spell.maxCritChance = s.spell.critChance[i];
    }
    s.spell.hasteRating      = p.GetDescU32(f::PLAYER_COMBAT_RATING + f::CR_HASTE_SPELL);
    s.spell.manaRegen5       = p.GetDescFloat(f::UNIT_POWER_REGEN) * 5.f;
    s.spell.manaRegen5Combat = p.GetDescFloat(f::UNIT_POWER_REGEN_COMBAT) * 5.f;

    // --- Defenses ---
    s.defense.armor         = s.base.armor;
    s.defense.defenseSkill  = ReadDefenseSkill(p);
    s.defense.defenseRating = p.GetDescU32(f::PLAYER_COMBAT_RATING + f::CR_DEFENSE_SKILL);
    s.defense.dodge         = p.GetDescFloat(f::PLAYER_DODGE_PERCENTAGE);
    s.defense.parry         = p.GetDescFloat(f::PLAYER_PARRY_PERCENTAGE);
    s.defense.block         = p.GetDescFloat(f::PLAYER_BLOCK_PERCENTAGE);
    s.defense.resilience    = p.GetDescU32(f::PLAYER_COMBAT_RATING + 15); // CR_CRIT_TAKEN_MELEE
    s.defense.shieldBlock   = p.GetDescU32(f::PLAYER_SHIELD_BLOCK);

    // --- PvP ---
    s.pvp.honorCurrency = p.GetDescU32(f::PLAYER_HONOR_CURRENCY);
    s.pvp.arenaCurrency = p.GetDescU32(f::PLAYER_ARENA_CURRENCY);
    s.pvp.kills         = p.GetDescU32(f::PLAYER_KILLS);
    s.pvp.lifetimeHKs   = p.GetDescU32(f::PLAYER_LIFETIME_HKS);

    return s;
}

} // namespace game
