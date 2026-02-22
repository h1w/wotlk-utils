#pragma once
#include <cstdint>

namespace mapedit {

struct MapInfo {
    uint32_t id;
    const char* name;
};

inline constexpr MapInfo kMaps[] = {
    {0,   "Eastern Kingdoms"},
    {1,   "Kalimdor"},
    {13,  "Testing"},
    {25,  "Scott Test"},
    {29,  "CashTest"},
    {30,  "Alterac Valley"},
    {33,  "Shadowfang Keep"},
    {34,  "Stormwind Stockade"},
    {36,  "Deadmines"},
    {43,  "Wailing Caverns"},
    {44,  "Monastery"},
    {47,  "Razorfen Kraul"},
    {48,  "Blackfathom Deeps"},
    {70,  "Uldaman"},
    {90,  "Gnomeregan"},
    {109, "Sunken Temple"},
    {129, "Razorfen Downs"},
    {189, "Scarlet Monastery"},
    {209, "Zul'Farrak"},
    {229, "Blackrock Spire"},
    {230, "Blackrock Depths"},
    {249, "Onyxia's Lair"},
    {269, "Opening of the Dark Portal"},
    {289, "Scholomance"},
    {309, "Zul'Gurub"},
    {329, "Stratholme"},
    {349, "Maraudon"},
    {369, "Deeprun Tram"},
    {389, "Ragefire Chasm"},
    {409, "Molten Core"},
    {429, "Dire Maul"},
    {449, "Alliance PVP Barracks"},
    {450, "Horde PVP Barracks"},
    {469, "Blackwing Lair"},
    {489, "Warsong Gulch"},
    {509, "Ruins of Ahn'Qiraj"},
    {529, "Arathi Basin"},
    {530, "Outland"},
    {531, "Ahn'Qiraj Temple"},
    {532, "Karazhan"},
    {533, "Naxxramas"},
    {534, "The Battle for Mount Hyjal"},
    {540, "Hellfire Ramparts"},
    {542, "Blood Furnace"},
    {543, "Hellfire Citadel"},
    {544, "Magtheridon's Lair"},
    {545, "The Steamvault"},
    {546, "The Underbog"},
    {547, "The Slave Pens"},
    {548, "Serpentshrine Cavern"},
    {550, "Tempest Keep"},
    {552, "The Arcatraz"},
    {553, "The Botanica"},
    {554, "The Mechanar"},
    {555, "Shadow Labyrinth"},
    {556, "Sethekk Halls"},
    {557, "Mana-Tombs"},
    {558, "Auchenai Crypts"},
    {560, "Old Hillsbrad Foothills"},
    {562, "Blade's Edge Arena"},
    {564, "Black Temple"},
    {565, "Gruul's Lair"},
    {566, "Eye of the Storm"},
    {568, "Zul'Aman"},
    {571, "Northrend"},
    {574, "Utgarde Keep"},
    {575, "Utgarde Pinnacle"},
    {576, "The Nexus"},
    {578, "The Oculus"},
    {580, "Sunwell Plateau"},
    {585, "Magisters' Terrace"},
    {595, "The Culling of Stratholme"},
    {599, "Halls of Stone"},
    {600, "Drak'Tharon Keep"},
    {601, "Azjol-Nerub"},
    {602, "Halls of Lightning"},
    {603, "Ulduar"},
    {604, "Gundrak"},
    {607, "Strand of the Ancients"},
    {608, "Violet Hold"},
    {615, "The Obsidian Sanctum"},
    {616, "The Eye of Eternity"},
    {619, "Ahn'kahet: The Old Kingdom"},
    {624, "Vault of Archavon"},
    {628, "Isle of Conquest"},
    {631, "Icecrown Citadel"},
    {632, "The Forge of Souls"},
    {649, "Trial of the Crusader"},
    {650, "Trial of the Champion"},
    {658, "Pit of Saron"},
    {668, "Halls of Reflection"},
    {724, "The Ruby Sanctum"},
};

inline constexpr int kMapCount = sizeof(kMaps) / sizeof(kMaps[0]);

inline const MapInfo* FindMap(uint32_t id) {
    for (int i = 0; i < kMapCount; ++i)
        if (kMaps[i].id == id) return &kMaps[i];
    return nullptr;
}

} // namespace mapedit
