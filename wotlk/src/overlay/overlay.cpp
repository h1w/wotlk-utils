#include "overlay.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d9.h>

#include <imgui.h>
#include <imgui_impl_dx9.h>
#include <imgui_impl_win32.h>

#include <MinHook.h>
#include <glog/logging.h>

#include "../game/game.h"
#include "../game/world.h"
#include "../game/spell.h"
#include "../game/player_stats.h"
#include "../offsets/offsets.h"
#include "../bot/action_queue.h"
#include "../bot/tools/wait.h"
#include "../bot/tools/move_to.h"
#include "../bot/tools/attack.h"
#include "../bot/tools/use_spell.h"
#include "../bot/tools/sequence.h"
#include "../bot/tools/loot.h"
#include "../bot/tools/interact.h"
#include "../navigation/nav_mesh.h"
#include "../navigation/pathfinder.h"
#include "../bot/tools/follow_route.h"
#include "../bot/tools/road_nav.h"
#include "../bot/radar.h"
#include "../bot/threat_scanner.h"
#include "../bot/aggro.h"

#include <cmath>

// Shared state: Navigate tab destination (accessible from radar context menu)
static float s_destPos[3] = { 0, 0, 0 };

// Forward declaration from imgui_impl_win32.cpp
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

// ---- Race / Class name lookups (WotLK 3.3.5a) ----

static const char* GetRaceName(uint8_t race)
{
    switch (race) {
    case 1:  return "Human";    case 2:  return "Orc";
    case 3:  return "Dwarf";    case 4:  return "Night Elf";
    case 5:  return "Undead";   case 6:  return "Tauren";
    case 7:  return "Gnome";    case 8:  return "Troll";
    case 10: return "Blood Elf"; case 11: return "Draenei";
    default: return "Unknown";
    }
}

static const char* GetClassName(uint8_t classId)
{
    switch (classId) {
    case 1:  return "Warrior";  case 2:  return "Paladin";
    case 3:  return "Hunter";   case 4:  return "Rogue";
    case 5:  return "Priest";   case 6:  return "Death Knight";
    case 7:  return "Shaman";   case 8:  return "Mage";
    case 9:  return "Warlock";  case 11: return "Druid";
    default: return "Unknown";
    }
}

static ImVec4 PowerColor(uint8_t classId)
{
    switch (classId) {
    case 1:  return ImVec4(0.8f, 0.0f, 0.0f, 1.0f); // Rage = red
    case 4:  return ImVec4(1.0f, 1.0f, 0.0f, 1.0f); // Energy = yellow
    case 6:  return ImVec4(0.0f, 0.8f, 0.8f, 1.0f); // Runic = cyan
    default: return ImVec4(0.0f, 0.4f, 1.0f, 1.0f); // Mana = blue
    }
}

static void RenderPlayerInfoWidget()
{
    ImGui::Begin("wotlk-utils");

    if (!game::world::IsInGame()) {
        ImGui::TextDisabled("Not in game");
        ImGui::End();
        return;
    }

    auto player = game::GetLocalPlayer();
    if (!player) {
        ImGui::TextDisabled("No player");
        ImGui::End();
        return;
    }

    // Gather all stats in one snapshot
    auto s = game::GatherPlayerStats(*player);

    const auto& i = s.info;
    const auto& r = s.resources;

    // Realm + Identity
    if (!i.realm.empty())
        ImGui::Text("Realm: %s", i.realm.c_str());
    ImGui::Text("%s", i.name.c_str());
    ImGui::SameLine();
    ImGui::TextDisabled("Lv %u %s %s", i.level, GetRaceName(i.race), GetClassName(i.classId));
    ImGui::Separator();

    // Health bar
    if (r.maxHealth > 0) {
        float frac = static_cast<float>(r.health) / static_cast<float>(r.maxHealth);
        char lbl[64]; snprintf(lbl, sizeof(lbl), "HP: %u / %u", r.health, r.maxHealth);
        ImGui::PushStyleColor(ImGuiCol_PlotHistogram, ImVec4(0.f, 0.8f, 0.f, 1.f));
        ImGui::ProgressBar(frac, ImVec2(-1, 0), lbl);
        ImGui::PopStyleColor();
    }

    // Power bar
    if (r.maxPower > 0) {
        float frac = static_cast<float>(r.power) / static_cast<float>(r.maxPower);
        char lbl[64]; snprintf(lbl, sizeof(lbl), "%s: %u / %u", r.powerLabel, r.power, r.maxPower);
        ImGui::PushStyleColor(ImGuiCol_PlotHistogram, PowerColor(i.classId));
        ImGui::ProgressBar(frac, ImVec2(-1, 0), lbl);
        ImGui::PopStyleColor();
    }
    ImGui::Separator();

    // XP bar
    if (r.nextLevelXp > 0 && i.level < 80) {
        float frac = static_cast<float>(r.xp) / static_cast<float>(r.nextLevelXp);
        char lbl[64]; snprintf(lbl, sizeof(lbl), "XP: %u / %u (%.1f%%)", r.xp, r.nextLevelXp, frac * 100.f);
        ImGui::PushStyleColor(ImGuiCol_PlotHistogram, ImVec4(0.5f, 0.f, 0.8f, 1.f));
        ImGui::ProgressBar(frac, ImVec2(-1, 0), lbl);
        ImGui::PopStyleColor();
    } else if (i.level >= 80) {
        ImGui::TextDisabled("XP: Max Level");
    }

    // Money
    ImGui::Text("Money: %ug %us %uc", r.coinage / 10000, (r.coinage % 10000) / 100, r.coinage % 100);
    ImGui::Separator();

    // Location
    const auto& loc = s.location;
    if (!loc.zone.empty()) {
        if (!loc.subZone.empty())
            ImGui::Text("Zone: %s - %s", loc.zone.c_str(), loc.subZone.c_str());
        else
            ImGui::Text("Zone: %s", loc.zone.c_str());
    }
    ImGui::Text("Pos: %.1f, %.1f, %.1f", loc.position.x, loc.position.y, loc.position.z);

    // ================================================================
    // Stats
    // ================================================================
    static const ImVec4 hdrCol(1.f, 0.8f, 0.2f, 1.f);

    if (ImGui::CollapsingHeader("Stats")) {
        // ---- Base Stats ----
        ImGui::TextColored(hdrCol, "Base Stats");
        ImGui::Text("Strength:  %u", s.base.strength);
        ImGui::Text("Agility:   %u", s.base.agility);
        ImGui::Text("Stamina:   %u", s.base.stamina);
        ImGui::Text("Intellect: %u", s.base.intellect);
        ImGui::Text("Spirit:    %u", s.base.spirit);
        ImGui::Text("Armor:     %u", s.base.armor);
        if (s.base.holyRes || s.base.fireRes || s.base.natureRes ||
            s.base.frostRes || s.base.shadowRes || s.base.arcaneRes) {
            ImGui::Text("Holy: %u  Fire: %u  Nature: %u", s.base.holyRes, s.base.fireRes, s.base.natureRes);
            ImGui::Text("Frost: %u  Shadow: %u  Arcane: %u", s.base.frostRes, s.base.shadowRes, s.base.arcaneRes);
        }
        ImGui::Spacing();

        // ---- Melee ----
        ImGui::TextColored(hdrCol, "Melee");
        ImGui::Text("Damage:      %.0f - %.0f", s.melee.minDamage, s.melee.maxDamage);
        if (s.melee.minOffhandDmg > 0 || s.melee.maxOffhandDmg > 0)
            ImGui::Text("Offhand:     %.0f - %.0f", s.melee.minOffhandDmg, s.melee.maxOffhandDmg);
        ImGui::Text("Speed:       %.2f", s.melee.speed);
        ImGui::Text("Power:       %d", s.melee.attackPower);
        ImGui::Text("Hit Rating:  %u", s.melee.hitRating);
        ImGui::Text("Crit Chance: %.2f%%", s.melee.critChance);
        ImGui::Text("Expertise:   %u", s.melee.expertise);
        if (s.melee.hasteRating)   ImGui::Text("Haste Rating:    %u", s.melee.hasteRating);
        if (s.melee.armorPenRating) ImGui::Text("Armor Pen:       %u", s.melee.armorPenRating);
        ImGui::Spacing();

        // ---- Ranged ----
        ImGui::TextColored(hdrCol, "Ranged");
        ImGui::Text("Damage:      %.0f - %.0f", s.ranged.minDamage, s.ranged.maxDamage);
        ImGui::Text("Speed:       %.2f", s.ranged.speed);
        ImGui::Text("Power:       %d", s.ranged.attackPower);
        ImGui::Text("Hit Rating:  %u", s.ranged.hitRating);
        ImGui::Text("Crit Chance: %.2f%%", s.ranged.critChance);
        if (s.ranged.hasteRating) ImGui::Text("Haste Rating:    %u", s.ranged.hasteRating);
        ImGui::Spacing();

        // ---- Spell ----
        ImGui::TextColored(hdrCol, "Spell");
        ImGui::Text("Bonus Damage:  %d", s.spell.maxBonusDamage);
        ImGui::Text("Bonus Healing: %d", s.spell.bonusHealing);
        ImGui::Text("Hit Rating:    %u", s.spell.hitRating);
        ImGui::Text("Crit Chance:   %.2f%%", s.spell.maxCritChance);
        ImGui::Text("Haste Rating:  %u", s.spell.hasteRating);
        ImGui::Text("Mana Regen:    %d", static_cast<int>(s.spell.manaRegen5 + 0.5f));
        ImGui::Text("MP5 (combat):  %d", static_cast<int>(s.spell.manaRegen5Combat + 0.5f));
        ImGui::Spacing();

        // ---- Defenses ----
        ImGui::TextColored(hdrCol, "Defenses");
        ImGui::Text("Armor:      %u", s.defense.armor);
        ImGui::Text("Defense:    %u", s.defense.defenseSkill);
        ImGui::Text("Dodge:      %.2f%%", s.defense.dodge);
        ImGui::Text("Parry:      %.2f%%", s.defense.parry);
        ImGui::Text("Block:      %.2f%%", s.defense.block);
        if (s.defense.shieldBlock) ImGui::Text("Block Value: %u", s.defense.shieldBlock);
        if (s.defense.resilience)  ImGui::Text("Resilience:  %u", s.defense.resilience);
        ImGui::Spacing();

        // ---- PvP ----
        if (s.pvp.honorCurrency || s.pvp.arenaCurrency || s.pvp.lifetimeHKs) {
            ImGui::TextColored(hdrCol, "PvP");
            ImGui::Text("Honor:      %u", s.pvp.honorCurrency);
            ImGui::Text("Arena Pts:  %u", s.pvp.arenaCurrency);
            ImGui::Text("Lifetime HKs: %u", s.pvp.lifetimeHKs);
        }
    }

    ImGui::End();
}

// ---- Bot Action Queue widget ----

static void RenderToolsTab()
{
    auto& queue = bot::ActionQueue::Instance();

    if (ImGui::BeginTabBar("ToolTabs")) {

        // ---- Wait ----
        if (ImGui::BeginTabItem("Wait")) {
            static int waitMs = 3000;
            ImGui::SliderInt("Duration (ms)", &waitMs, 500, 15000);
            if (ImGui::Button("PushBack"))
                queue.PushBack(std::make_unique<bot::WaitTool>(static_cast<uint32_t>(waitMs)));
            ImGui::SameLine();
            if (ImGui::Button("Interrupt"))
                queue.Interrupt(std::make_unique<bot::WaitTool>(static_cast<uint32_t>(waitMs)));
            ImGui::EndTabItem();
        }

        // ---- Attack ----
        if (ImGui::BeginTabItem("Attack")) {
            ImGui::TextWrapped("Attacks the current target (select a target in game first).");

            auto target = game::GetTarget();
            if (target) {
                ImGui::Text("Target: %s (HP: %.0f%%)",
                    target->GetUnitName().c_str(), target->GetHealthPercent());

                game::GUID guid = target->GetGUID();
                if (ImGui::Button("PushBack"))
                    queue.PushBack(std::make_unique<bot::AttackTool>(guid));
                ImGui::SameLine();
                if (ImGui::Button("Interrupt"))
                    queue.Interrupt(std::make_unique<bot::AttackTool>(guid));
            } else {
                ImGui::TextDisabled("No target selected");
            }
            ImGui::EndTabItem();
        }

        // ---- UseSpell ----
        if (ImGui::BeginTabItem("UseSpell")) {
            static int spellId = 0;
            ImGui::InputInt("Spell ID", &spellId);

            bool known = (spellId > 0) && game::spell::HasSpell(static_cast<uint32_t>(spellId));
            if (spellId > 0) {
                if (known)
                    ImGui::TextColored(ImVec4(0.2f, 1.f, 0.2f, 1.f), "Spell known");
                else
                    ImGui::TextColored(ImVec4(1.f, 0.3f, 0.3f, 1.f), "Spell NOT known");
            }

            if (known) {
                bool onCD = game::spell::IsOnCooldown(static_cast<uint32_t>(spellId));
                if (onCD)
                    ImGui::TextColored(ImVec4(1.f, 1.f, 0.2f, 1.f), "On cooldown");

                if (ImGui::Button("PushBack"))
                    queue.PushBack(std::make_unique<bot::UseSpellTool>(
                        static_cast<uint32_t>(spellId)));
                ImGui::SameLine();
                if (ImGui::Button("Interrupt"))
                    queue.Interrupt(std::make_unique<bot::UseSpellTool>(
                        static_cast<uint32_t>(spellId)));
            }
            ImGui::EndTabItem();
        }

        // ---- Loot ----
        if (ImGui::BeginTabItem("Loot")) {
            ImGui::TextWrapped("Loot the current target (must be dead).");

            auto target = game::GetTarget();
            if (target && target->IsDead()) {
                ImGui::Text("Target: %s (dead)", target->GetUnitName().c_str());
                game::GUID guid = target->GetGUID();
                if (ImGui::Button("PushBack"))
                    queue.PushBack(std::make_unique<bot::LootTool>(guid));
                ImGui::SameLine();
                if (ImGui::Button("Interrupt"))
                    queue.Interrupt(std::make_unique<bot::LootTool>(guid));
            } else if (target) {
                ImGui::TextDisabled("Target is alive — kill it first");
            } else {
                ImGui::TextDisabled("No target selected");
            }
            ImGui::EndTabItem();
        }

        // ---- Interact ----
        if (ImGui::BeginTabItem("Interact")) {
            ImGui::TextWrapped("Walk to and interact with current target/object.");

            auto target = game::GetTarget();
            if (target) {
                ImGui::Text("Target: %s", target->GetUnitName().c_str());
                game::GUID guid = target->GetGUID();
                if (ImGui::Button("PushBack"))
                    queue.PushBack(std::make_unique<bot::InteractTool>(guid));
                ImGui::SameLine();
                if (ImGui::Button("Interrupt"))
                    queue.Interrupt(std::make_unique<bot::InteractTool>(guid));
            } else {
                ImGui::TextDisabled("No target selected");
            }
            ImGui::EndTabItem();
        }

        // ---- Navigate ----
        if (ImGui::BeginTabItem("Navigate")) {
            auto& navMesh = nav::NavMesh::Instance();

            // NavMesh status
            if (navMesh.IsReady()) {
                ImGui::TextColored(ImVec4(0.2f, 1.f, 0.2f, 1.f), "NavMesh: ready (%d tiles)",
                                   navMesh.GetLoadedTileCount());
            } else {
                ImGui::TextColored(ImVec4(1.f, 0.3f, 0.3f, 1.f), "NavMesh: not loaded");
                ImGui::TextWrapped("Place mmaps in the mmaps/ folder next to the DLL.");
            }

            if (ImGui::Button("Use Target Pos")) {
                auto target = game::GetTarget();
                if (target) {
                    auto p = target->GetPosition();
                    s_destPos[0] = p.x; s_destPos[1] = p.y; s_destPos[2] = p.z;
                }
            }
            ImGui::SameLine();
            if (ImGui::Button("Use Current Pos")) {
                auto player = game::GetLocalPlayer();
                if (player) {
                    auto p = player->GetPosition();
                    s_destPos[0] = p.x; s_destPos[1] = p.y; s_destPos[2] = p.z;
                }
            }
            ImGui::InputFloat3("Destination", s_destPos);

            // Show distance
            auto player = game::GetLocalPlayer();
            if (player) {
                game::Vec3 dest{s_destPos[0], s_destPos[1], s_destPos[2]};
                float dist = player->GetPosition().DistanceTo(dest);
                ImGui::Text("Distance: %.0f yd", dist);
            }

            if (navMesh.IsReady()) {
                // Smart navigation: Tier 3 (strategic) > Tier 2 (road) > Tier 1 (navmesh)
                if (ImGui::Button("Find Path & Go")) {
                    game::Vec3 end{s_destPos[0], s_destPos[1], s_destPos[2]};
                    queue.PushBack(bot::MoveToTool::CreateSmart(end));
                }
                ImGui::SameLine();
                if (ImGui::Button("Interrupt & Go")) {
                    game::Vec3 end{s_destPos[0], s_destPos[1], s_destPos[2]};
                    queue.Interrupt(bot::MoveToTool::CreateSmart(end));
                }

                // Test: find path only (show info)
                if (ImGui::Button("Test Path (log only)")) {
                    auto p = game::GetLocalPlayer();
                    if (p) {
                        game::Vec3 start = p->GetPosition();
                        game::Vec3 end{s_destPos[0], s_destPos[1], s_destPos[2]};
                        auto result = nav::Pathfinder::Instance().FindPath(start, end);
                        if (result.success) {
                            float totalDist = 0.f;
                            for (size_t i = 1; i < result.waypoints.size(); ++i)
                                totalDist += result.waypoints[i-1].DistanceTo(result.waypoints[i]);
                            LOG(INFO) << "[Nav] Test: " << result.waypoints.size()
                                      << " waypoints, total distance: " << totalDist << " yd"
                                      << (result.partial ? " (PARTIAL)" : "");
                        } else {
                            LOG(WARNING) << "[Nav] Test: no path found";
                        }
                    }
                }
            }
            ImGui::EndTabItem();
        }

        // ---- Sequence ----
        if (ImGui::BeginTabItem("Kill & Loot")) {
            ImGui::TextWrapped("Combo: attack target until dead, then loot the corpse.");

            auto target = game::GetTarget();
            if (target && !target->IsDead()) {
                ImGui::Text("Target: %s (HP: %.0f%%)",
                    target->GetUnitName().c_str(), target->GetHealthPercent());

                game::GUID guid = target->GetGUID();
                game::Vec3 tpos = target->GetPosition();

                if (ImGui::Button("Kill & Loot")) {
                    std::vector<bot::ToolPtr> steps;
                    steps.push_back(std::make_unique<bot::AttackTool>(guid));
                    steps.push_back(std::make_unique<bot::LootTool>(guid));
                    queue.PushBack(std::make_unique<bot::SequenceTool>(std::move(steps)));
                }
                ImGui::SameLine();
                if (ImGui::Button("MoveTo + Kill + Loot")) {
                    std::vector<bot::ToolPtr> steps;
                    steps.push_back(std::make_unique<bot::MoveToTool>(tpos));
                    steps.push_back(std::make_unique<bot::AttackTool>(guid));
                    steps.push_back(std::make_unique<bot::LootTool>(guid));
                    queue.PushBack(std::make_unique<bot::SequenceTool>(std::move(steps)));
                }
            } else if (target && target->IsDead()) {
                ImGui::TextDisabled("Target is already dead");
            } else {
                ImGui::TextDisabled("No target selected");
            }
            ImGui::EndTabItem();
        }

        ImGui::EndTabBar();
    }
}

static void RenderQueueList()
{
    auto& queue = bot::ActionQueue::Instance();

    if (ImGui::Button("Clear All"))
        queue.Clear();

    ImGui::Separator();

    if (queue.IsEmpty()) {
        ImGui::TextDisabled("Queue is empty");
        return;
    }

    ImGui::Text("Queue: %zu tool(s)", queue.Size());
    ImGui::Separator();

    int removeIdx = -1;
    const auto& all = queue.GetAll();

    for (size_t i = 0; i < all.size(); ++i) {
        const auto* tool = all[i].get();
        auto status = tool->GetStatus();

        ImGui::PushID(static_cast<int>(i));

        // Status indicator
        if (i == 0 && status == bot::ToolStatus::Running)
            ImGui::TextColored(ImVec4(0.2f, 1.f, 0.2f, 1.f), ">>>");
        else
            ImGui::TextDisabled("   ");
        ImGui::SameLine();

        // Tool description
        ImGui::Text("[%zu] %s", i, tool->Describe().c_str());

        // Progress bar for WaitTool
        if (tool->GetType() == bot::ToolType::Wait && status == bot::ToolStatus::Running) {
            auto* wait = static_cast<const bot::WaitTool*>(tool);
            char lbl[64];
            snprintf(lbl, sizeof(lbl), "%llu / %u ms", wait->GetElapsedMs(), wait->GetDurationMs());
            ImGui::ProgressBar(wait->GetProgress(), ImVec2(-1, 0), lbl);
        }

        // Distance for MoveToTool
        if (tool->GetType() == bot::ToolType::MoveTo && status == bot::ToolStatus::Running) {
            auto* mt = static_cast<const bot::MoveToTool*>(tool);
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.8f, 0.8f, 0.2f, 1.f), "(%.0f yd)", mt->GetDistanceRemaining());
        }

        // Remove button
        ImGui::SameLine();
        if (ImGui::SmallButton("X"))
            removeIdx = static_cast<int>(i);

        ImGui::PopID();
    }

    if (removeIdx >= 0)
        queue.Remove(static_cast<size_t>(removeIdx));
}

static void RenderToolsWidget()
{
    ImGui::Begin("Tools");
    RenderToolsTab();
    ImGui::End();
}

static void RenderQueueWidget()
{
    ImGui::Begin("Queue");
    RenderQueueList();
    ImGui::End();
}

// ---- Radar widget ----

static ImVec2 WorldToRadar(const game::Vec3& worldPos, const game::Vec3& playerPos,
                           float playerFacing, ImVec2 center, float scale, bool facingUp)
{
    // WoW: X+ = south, Y+ = west.  Facing = atan2(dy,dx): 0=south, pi/2=west, pi=north.
    // Radar north-up: screenX = center - dy*scale (west=left), screenY = center + dx*scale (south=down)
    float dx = worldPos.x - playerPos.x;  // positive = south
    float dy = worldPos.y - playerPos.y;  // positive = west

    if (facingUp) {
        // Rotate by -(pi - facing) to account for WoW's left-handed XY coords
        // (X+=south, Y+=west → clockwise when viewed from above)
        float cosF = cosf(playerFacing);
        float sinF = sinf(playerFacing);
        float rx = -dx * cosF + dy * sinF;
        float ry = -dx * sinF - dy * cosF;
        dx = rx;
        dy = ry;
    }

    return ImVec2(center.x - dy * scale, center.y + dx * scale);
}

// Inverse of WorldToRadar: convert screen position back to world coordinates (2D, Z from player)
static game::Vec3 RadarToWorld(ImVec2 screenPos, const game::Vec3& playerPos,
                                float playerFacing, ImVec2 center, float scale, bool facingUp)
{
    // Invert: screenX = center.x - dy*scale, screenY = center.y + dx*scale
    float dx = (screenPos.y - center.y) / scale;
    float dy = (center.x - screenPos.x) / scale;

    if (facingUp) {
        // Invert rotation: forward is rx=-dx*cosF+dy*sinF, ry=-dx*sinF-dy*cosF
        // Inverse (det=1): dx=-cosF*rx-sinF*ry, dy=sinF*rx-cosF*ry
        float cosF = cosf(playerFacing);
        float sinF = sinf(playerFacing);
        float origDx = -cosF * dx - sinF * dy;
        float origDy =  sinF * dx - cosF * dy;
        dx = origDx;
        dy = origDy;
    }

    game::Vec3 result;
    result.x = playerPos.x + dx;
    result.y = playerPos.y + dy;
    result.z = playerPos.z; // Can't determine Z from 2D map
    return result;
}

static void DrawPlayerArrow(ImDrawList* dl, ImVec2 center, float facing, bool facingUp)
{
    // Triangle pointing in facing direction
    // WoW facing: 0=south, pi/2=west, pi=north.  Screen: 0=right, pi/2=down.
    // North-up: screenAngle = facing + pi/2.  FacingUp: always points up (-pi/2).
    float angle = facingUp ? (-3.14159265f / 2.0f) : (facing + 3.14159265f / 2.0f);

    const float sz = 8.0f;
    ImVec2 tip(center.x + cosf(angle) * sz, center.y + sinf(angle) * sz);
    ImVec2 l(center.x + cosf(angle + 2.5f) * sz * 0.7f, center.y + sinf(angle + 2.5f) * sz * 0.7f);
    ImVec2 r(center.x + cosf(angle - 2.5f) * sz * 0.7f, center.y + sinf(angle - 2.5f) * sz * 0.7f);

    dl->AddTriangleFilled(tip, l, r, IM_COL32(255, 255, 255, 230));
    dl->AddTriangle(tip, l, r, IM_COL32(0, 0, 0, 180), 1.0f);
}

static void DrawRangeCircles(ImDrawList* dl, ImVec2 center, float scale, float visRange, float clipR)
{
    const float ranges[] = { 10.f, 25.f, 50.f, 100.f };
    for (float r : ranges) {
        if (r > visRange) break;
        float px = r * scale;
        if (px > clipR) continue;
        dl->AddCircle(center, px, IM_COL32(255, 255, 255, 40), 64);
        // Label
        char lbl[16];
        snprintf(lbl, sizeof(lbl), "%dyd", static_cast<int>(r));
        dl->AddText(ImVec2(center.x + px + 2, center.y - 8), IM_COL32(255, 255, 255, 60), lbl);
    }
}

static ImU32 ReactionColor(const bot::RadarEntry& e, bool fill)
{
    uint8_t a = fill ? 200 : 255;
    if (e.objType == game::ObjectType::GameObject)
        return IM_COL32(255, 165, 0, a);   // orange
    if (e.isDead)
        return IM_COL32(128, 128, 128, a);  // grey
    if (e.isPlayer)
        return IM_COL32(80, 140, 255, a);   // blue
    switch (e.reaction) {
    case game::UnitReaction::Hostile:
    case game::UnitReaction::Unfriendly:
        return IM_COL32(255, 60, 60, a);    // red
    case game::UnitReaction::Friendly:
    case game::UnitReaction::Honored:
        return IM_COL32(60, 255, 60, a);    // green
    default:
        return IM_COL32(255, 255, 60, a);   // yellow (neutral)
    }
}

static void RenderRadarWidget()
{
    if (!game::world::IsInGame()) return;
    auto player = game::GetLocalPlayer();
    if (!player) return;

    // Persistent state
    auto& s_radarData = bot::RadarData::Instance();
    static float  s_visibleRange  = 80.f;
    static bool   s_facingUp      = false;
    static bool   s_showHostiles  = true;
    static bool   s_showFriendlies = true;
    static bool   s_showNeutrals  = true;
    static bool   s_showPlayers   = true;
    static bool   s_showGameObjects = true;
    static bool   s_showPath      = true;
    static bool   s_showAggro     = true;
    static bool   s_showDead      = false;

    s_radarData.Update(*player);

    ImGui::Begin("Radar");

    // Canvas size
    float canvasSize = ImGui::GetContentRegionAvail().x;
    if (canvasSize < 100.f) canvasSize = 200.f;
    if (canvasSize > 600.f) canvasSize = 600.f;
    float halfCanvas = canvasSize * 0.5f;

    ImVec2 canvasPos = ImGui::GetCursorScreenPos();
    ImVec2 center(canvasPos.x + halfCanvas, canvasPos.y + halfCanvas);

    // Reserve canvas space
    ImGui::InvisibleButton("radar_canvas", ImVec2(canvasSize, canvasSize),
                           ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
    bool canvasHovered = ImGui::IsItemHovered();
    bool canvasRightClicked = ImGui::IsItemClicked(ImGuiMouseButton_Right);

    ImDrawList* dl = ImGui::GetWindowDrawList();

    // Clip to canvas
    dl->PushClipRect(canvasPos, ImVec2(canvasPos.x + canvasSize, canvasPos.y + canvasSize), true);

    // 1. Background
    dl->AddRectFilled(canvasPos, ImVec2(canvasPos.x + canvasSize, canvasPos.y + canvasSize),
                      IM_COL32(20, 20, 25, 220));

    float scale = halfCanvas / s_visibleRange;  // pixels per yard

    game::Vec3 myPos = player->GetPosition();
    float myFacing = player->GetFacing();

    // 2. Range circles
    DrawRangeCircles(dl, center, scale, s_visibleRange, halfCanvas);

    const auto& entries = s_radarData.GetEntries();

    // Track hovered entry for tooltip
    const bot::RadarEntry* hoveredEntry = nullptr;
    float hoveredDistSq = 64.f; // 8px threshold squared

    // 3. Aggro zones (back layer)
    if (s_showAggro) {
        for (const auto& e : entries) {
            if (e.aggroRadiusBuffered <= 0.f || e.isDead) continue;
            if (!s_showHostiles) continue;
            ImVec2 sp = WorldToRadar(e.position, myPos, myFacing, center, scale, s_facingUp);
            float rPx = e.aggroRadiusBuffered * scale;
            dl->AddCircleFilled(sp, rPx, IM_COL32(255, 40, 40, 25), 32);
            dl->AddCircle(sp, rPx, IM_COL32(255, 40, 40, 50), 32);
        }
    }

    // 4. Nav path + threat visualization
    const std::vector<game::Vec3>* detourWPs = nullptr;
    bool isInForcedCombat = false;

    if (s_showPath) {
        auto* current = bot::ActionQueue::Instance().GetCurrent();
        const std::vector<game::Vec3>* waypoints = nullptr;
        size_t currentWpIdx = 0;
        game::Vec3 destination{};
        bool hasPath = false;

        // Unwrap Sequence to find the active sub-tool
        const bot::ITool* activeTool = current;
        if (activeTool && activeTool->GetType() == bot::ToolType::Sequence) {
            auto* seq = static_cast<const bot::SequenceTool*>(activeTool);
            activeTool = seq->GetCurrentStep();
        }

        if (activeTool) {
            if (activeTool->GetType() == bot::ToolType::FollowRoute) {
                auto* fr = static_cast<const bot::FollowRouteTool*>(activeTool);
                waypoints = &fr->GetWaypoints();
                currentWpIdx = fr->GetCurrentWaypointIndex();
                if (!waypoints->empty()) {
                    destination = waypoints->back();
                    hasPath = true;
                }
                detourWPs = &fr->GetDetourWaypoints();
                isInForcedCombat = fr->IsInForcedCombat();
            } else if (activeTool->GetType() == bot::ToolType::MoveTo) {
                auto* mt = static_cast<const bot::MoveToTool*>(activeTool);
                waypoints = &mt->GetNavWaypoints();
                currentWpIdx = mt->GetNavCurrentIndex();
                destination = mt->GetTarget();
                hasPath = !waypoints->empty();
                detourWPs = &mt->GetDetourWaypoints();
                isInForcedCombat = mt->IsInForcedCombat();
            } else if (activeTool->GetType() == bot::ToolType::RoadNav) {
                auto* rn = static_cast<const bot::RoadNavTool*>(activeTool);
                waypoints = &rn->GetNavWaypoints();
                currentWpIdx = rn->GetNavCurrentIndex();
                destination = rn->GetDestination();
                hasPath = !waypoints->empty();
                detourWPs = &rn->GetDetourWaypoints();
            }
        }

        if (hasPath && waypoints && waypoints->size() >= 2) {
            // Draw polyline from current waypoint onward
            for (size_t i = currentWpIdx; i + 1 < waypoints->size(); ++i) {
                ImVec2 a = WorldToRadar((*waypoints)[i], myPos, myFacing, center, scale, s_facingUp);
                ImVec2 b = WorldToRadar((*waypoints)[i + 1], myPos, myFacing, center, scale, s_facingUp);
                dl->AddLine(a, b, IM_COL32(60, 255, 60, 140), 2.0f);
            }

            // Current waypoint marker
            if (currentWpIdx < waypoints->size()) {
                ImVec2 wp = WorldToRadar((*waypoints)[currentWpIdx], myPos, myFacing, center, scale, s_facingUp);
                dl->AddCircle(wp, 5.f, IM_COL32(60, 255, 60, 200), 12, 2.0f);
            }

            // Destination marker (gold X)
            ImVec2 dp = WorldToRadar(destination, myPos, myFacing, center, scale, s_facingUp);
            const float xsz = 5.f;
            dl->AddLine(ImVec2(dp.x - xsz, dp.y - xsz), ImVec2(dp.x + xsz, dp.y + xsz),
                        IM_COL32(255, 215, 0, 230), 2.0f);
            dl->AddLine(ImVec2(dp.x + xsz, dp.y - xsz), ImVec2(dp.x - xsz, dp.y + xsz),
                        IM_COL32(255, 215, 0, 230), 2.0f);
        }
    }

    // 4b. Detour waypoints (yellow dots)
    if (detourWPs && !detourWPs->empty()) {
        for (const auto& dwp : *detourWPs) {
            ImVec2 dp = WorldToRadar(dwp, myPos, myFacing, center, scale, s_facingUp);
            dl->AddCircleFilled(dp, 4.f, IM_COL32(255, 255, 0, 200), 12);
            dl->AddCircle(dp, 4.f, IM_COL32(0, 0, 0, 150), 12, 1.0f);
        }
    }

    // 4c. Pulsing circles on path-blocking NPCs
    if (s_showAggro && s_showPath) {
        // Use time for pulsing alpha (sinusoidal, 2Hz)
        float pulseT = static_cast<float>(GetTickCount64() % 1000) / 1000.0f;
        float pulseAlpha = 0.3f + 0.7f * (0.5f + 0.5f * sinf(pulseT * 6.2832f * 2.0f));
        uint8_t pulseA = static_cast<uint8_t>(pulseAlpha * 180.0f);

        auto* current = bot::ActionQueue::Instance().GetCurrent();
        const bot::ITool* activeTool = current;
        if (activeTool && activeTool->GetType() == bot::ToolType::Sequence) {
            auto* seq = static_cast<const bot::SequenceTool*>(activeTool);
            activeTool = seq->GetCurrentStep();
        }

        // Get blocking threats from active MoveToTool's NavHelper
        const std::vector<bot::BlockingThreat>* blockingThreats = nullptr;
        if (activeTool && activeTool->GetType() == bot::ToolType::MoveTo) {
            // We can't access NavHelper's blocking threats directly from const MoveToTool*,
            // so we scan entries that overlap with detour waypoints for visual indication.
            // The pulsing effect on any hostile NPC near the path serves the same purpose.
        }

        // Pulse any hostile NPC that would block a segment of the current path
        if (detourWPs && !detourWPs->empty()) {
            for (const auto& e : entries) {
                if (e.aggroRadiusBuffered <= 0.f || e.isDead || e.isPlayer) continue;
                // Check if this NPC is near any detour waypoint (it was rerouted around)
                for (const auto& dwp : *detourWPs) {
                    if (e.position.Distance2D(dwp) < e.aggroRadiusBuffered + 5.0f) {
                        ImVec2 sp = WorldToRadar(e.position, myPos, myFacing, center, scale, s_facingUp);
                        float rPx = e.aggroRadiusBuffered * scale;
                        dl->AddCircle(sp, rPx, IM_COL32(255, 0, 0, pulseA), 32, 2.5f);
                        break;
                    }
                }
            }
        }
    }

    // 4d. "FORCED COMBAT" text overlay
    if (isInForcedCombat) {
        const char* combatText = "FORCED COMBAT";
        ImVec2 textSize = ImGui::CalcTextSize(combatText);
        float textX = center.x - textSize.x * 0.5f;
        float textY = canvasPos.y + canvasSize - 20.f;
        dl->AddText(ImVec2(textX + 1, textY + 1), IM_COL32(0, 0, 0, 200), combatText);
        dl->AddText(ImVec2(textX, textY), IM_COL32(255, 60, 60, 255), combatText);
    }

    // 5-10. Draw entries (sorted far-to-near so near entries draw on top)
    ImVec2 mousePos = ImGui::GetMousePos();

    for (const auto& e : entries) {
        // Filter by show flags
        if (e.objType == game::ObjectType::GameObject) {
            if (!s_showGameObjects) continue;
        } else if (e.isDead) {
            if (!s_showDead) continue;
        } else if (e.isPlayer) {
            if (!s_showPlayers) continue;
        } else {
            switch (e.reaction) {
            case game::UnitReaction::Hostile:
            case game::UnitReaction::Unfriendly:
                if (!s_showHostiles) continue;
                break;
            case game::UnitReaction::Friendly:
            case game::UnitReaction::Honored:
                if (!s_showFriendlies) continue;
                break;
            default:
                if (!s_showNeutrals) continue;
                break;
            }
        }

        ImVec2 sp = WorldToRadar(e.position, myPos, myFacing, center, scale, s_facingUp);

        // Cull entries far outside canvas
        if (sp.x < canvasPos.x - 20 || sp.x > canvasPos.x + canvasSize + 20 ||
            sp.y < canvasPos.y - 20 || sp.y > canvasPos.y + canvasSize + 20)
            continue;

        ImU32 col = ReactionColor(e, true);

        if (e.objType == game::ObjectType::GameObject) {
            // Orange square
            const float sz = 3.f;
            dl->AddRectFilled(ImVec2(sp.x - sz, sp.y - sz), ImVec2(sp.x + sz, sp.y + sz), col);
        } else if (e.isDead) {
            // Grey outline circle
            dl->AddCircle(sp, 3.f, col, 12, 1.0f);
        } else if (e.isPlayer) {
            // Blue diamond
            const float sz = 4.f;
            ImVec2 pts[4] = {
                ImVec2(sp.x, sp.y - sz), ImVec2(sp.x + sz, sp.y),
                ImVec2(sp.x, sp.y + sz), ImVec2(sp.x - sz, sp.y),
            };
            dl->AddConvexPolyFilled(pts, 4, col);
            dl->AddPolyline(pts, 4, IM_COL32(0, 0, 0, 150), ImDrawFlags_Closed, 1.0f);
        } else {
            // NPC dot
            float r = (e.reaction == game::UnitReaction::Hostile ||
                       e.reaction == game::UnitReaction::Unfriendly) ? 4.f : 3.f;
            dl->AddCircleFilled(sp, r, col, 12);
            dl->AddCircle(sp, r, IM_COL32(0, 0, 0, 120), 12, 1.0f);
        }

        // Hit test for tooltip
        if (canvasHovered) {
            float dxM = mousePos.x - sp.x;
            float dyM = mousePos.y - sp.y;
            float dSq = dxM * dxM + dyM * dyM;
            if (dSq < hoveredDistSq) {
                hoveredDistSq = dSq;
                hoveredEntry = &e;
            }
        }
    }

    // 13. Player arrow (always on top, at center)
    DrawPlayerArrow(dl, center, myFacing, s_facingUp);

    // 14. North indicator
    if (s_facingUp) {
        // In facing-up mode, north rotates based on facing.
        // WoW facing 0=south, so north is at angle (facing).
        // Screen pos: x = center + sin(facing)*R, y = center + cos(facing)*R
        float nR = halfCanvas - 12.f;
        ImVec2 nPos(center.x + sinf(myFacing) * nR,
                    center.y + cosf(myFacing) * nR);
        dl->AddText(ImVec2(nPos.x - 3, nPos.y - 6), IM_COL32(255, 80, 80, 200), "N");
    } else {
        // North-up: N at top center
        dl->AddText(ImVec2(center.x - 3, canvasPos.y + 2), IM_COL32(255, 80, 80, 200), "N");
    }

    dl->PopClipRect();

    // Tooltip (only when context menu is NOT open)
    if (hoveredEntry && !ImGui::IsPopupOpen("radar_ctx")) {
        ImGui::BeginTooltip();
        ImGui::Text("%s", hoveredEntry->name.c_str());
        if (hoveredEntry->objType != game::ObjectType::GameObject) {
            ImGui::Text("Level %d  HP: %.0f%%", hoveredEntry->level, hoveredEntry->healthPct);
            if (hoveredEntry->isDead)
                ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.f), "Dead");
            if (hoveredEntry->isInCombat)
                ImGui::TextColored(ImVec4(1.f, 0.3f, 0.3f, 1.f), "In Combat");
        }
        ImGui::Text("Distance: %.0f yd", hoveredEntry->distToPlayer);
        ImGui::EndTooltip();
    }

    // Right-click context menu
    // Store value copy (not pointer) — RadarData::Update() may reallocate the entries vector
    static bool s_ctxHasEntry = false;
    static bot::RadarEntry s_ctxEntry;
    static ImVec2 s_ctxClickPos{};

    if (canvasRightClicked) {
        if (hoveredEntry) {
            s_ctxHasEntry = true;
            s_ctxEntry = *hoveredEntry;
        } else {
            s_ctxHasEntry = false;
        }
        s_ctxClickPos = ImGui::GetMousePos();
        ImGui::OpenPopup("radar_ctx");
    }

    if (ImGui::BeginPopup("radar_ctx")) {
        if (s_ctxHasEntry) {
            ImGui::TextDisabled("%s", s_ctxEntry.name.c_str());
            ImGui::Separator();
            if (ImGui::MenuItem("Target")) {
                game::SelectTarget(s_ctxEntry.guid);
            }
            if (ImGui::MenuItem("Set Coords")) {
                s_destPos[0] = s_ctxEntry.position.x;
                s_destPos[1] = s_ctxEntry.position.y;
                s_destPos[2] = s_ctxEntry.position.z;
            }
        } else {
            // Clicked on empty space — convert screen pos to world coords
            game::Vec3 worldClick = RadarToWorld(
                s_ctxClickPos, myPos, myFacing, center, scale, s_facingUp);
            ImGui::TextDisabled("(%.0f, %.0f)", worldClick.x, worldClick.y);
            ImGui::Separator();
            if (ImGui::MenuItem("Set Coords")) {
                s_destPos[0] = worldClick.x;
                s_destPos[1] = worldClick.y;
                s_destPos[2] = worldClick.z;
            }
        }
        ImGui::EndPopup();
    }

    // 15. Stats text
    ImGui::Text("H:%d  F:%d  N:%d  P:%d  O:%d",
        s_radarData.hostileCount, s_radarData.friendlyCount,
        s_radarData.neutralCount, s_radarData.playerCount,
        s_radarData.objectCount);

    // Controls
    ImGui::SliderFloat("Range", &s_visibleRange, 10.f, 200.f, "%.0f yd");

    if (s_facingUp) {
        if (ImGui::Button("Player-Facing-Up")) s_facingUp = false;
    } else {
        if (ImGui::Button("North-Up")) s_facingUp = true;
    }

    if (ImGui::TreeNode("Filters")) {
        ImGui::Checkbox("Hostiles", &s_showHostiles);
        ImGui::SameLine();
        ImGui::Checkbox("Friendly", &s_showFriendlies);
        ImGui::SameLine();
        ImGui::Checkbox("Neutral", &s_showNeutrals);
        ImGui::Checkbox("Players", &s_showPlayers);
        ImGui::SameLine();
        ImGui::Checkbox("Objects", &s_showGameObjects);
        ImGui::SameLine();
        ImGui::Checkbox("Dead", &s_showDead);
        ImGui::Checkbox("Aggro", &s_showAggro);
        ImGui::SameLine();
        ImGui::Checkbox("Path", &s_showPath);
        ImGui::TreePop();
    }

    ImGui::End();
}

namespace overlay {

// EndScene: index 42 in IDirect3DDevice9 vtable
using EndScene_t = HRESULT(WINAPI*)(IDirect3DDevice9*);
// Reset: index 16 in IDirect3DDevice9 vtable
using Reset_t = HRESULT(WINAPI*)(IDirect3DDevice9*, D3DPRESENT_PARAMETERS*);

static EndScene_t g_originalEndScene = nullptr;
static Reset_t g_originalReset = nullptr;
static void* g_endSceneTarget = nullptr;
static void* g_resetTarget = nullptr;
static HWND g_gameHwnd = nullptr;
static WNDPROC g_originalWndProc = nullptr;
static bool g_imguiInitialized = false;
static bool g_shutdownRequested = false;

// ---- Input via WndProc subclass ----

static LRESULT CALLBACK HookedWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (!g_shutdownRequested && g_imguiInitialized) {
        if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam))
            return 1; // ImGui consumed the message

        ImGuiIO& io = ImGui::GetIO();

        // Block input from reaching the game when ImGui wants it
        switch (msg) {
        case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_LBUTTONDBLCLK:
        case WM_RBUTTONDOWN: case WM_RBUTTONUP: case WM_RBUTTONDBLCLK:
        case WM_MBUTTONDOWN: case WM_MBUTTONUP: case WM_MBUTTONDBLCLK:
        case WM_MOUSEWHEEL: case WM_MOUSEHWHEEL:
            if (io.WantCaptureMouse)
                return 0;
            break;
        case WM_KEYDOWN: case WM_KEYUP:
        case WM_CHAR: case WM_SYSKEYDOWN: case WM_SYSKEYUP:
            if (io.WantCaptureKeyboard)
                return 0;
            break;
        }
    }

    return CallWindowProcA(g_originalWndProc, hWnd, msg, wParam, lParam);
}

// ---- EndScene hook ----

static HRESULT WINAPI HookedEndScene(IDirect3DDevice9* pDevice)
{
    if (g_shutdownRequested)
        return g_originalEndScene(pDevice);

    // One-time ImGui initialization (deferred to first EndScene to get pDevice + correct HWND)
    if (!g_imguiInitialized) {
        // Get the real game window from the D3D device
        D3DDEVICE_CREATION_PARAMETERS cp;
        if (SUCCEEDED(pDevice->GetCreationParameters(&cp)) && cp.hFocusWindow)
            g_gameHwnd = cp.hFocusWindow;

        if (!g_gameHwnd) {
            LOG(ERROR) << "[OVERLAY] Could not determine game HWND from D3D device";
            return g_originalEndScene(pDevice);
        }

        ImGui::CreateContext();
        ImGui_ImplWin32_Init(g_gameHwnd);
        ImGui_ImplDX9_Init(pDevice);

        ImGuiIO& io = ImGui::GetIO();
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

        // Subclass the window procedure for input
        g_originalWndProc = reinterpret_cast<WNDPROC>(
            SetWindowLongPtrA(g_gameHwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&HookedWndProc)));

        g_imguiInitialized = true;
        LOG(INFO) << "[OVERLAY] ImGui initialized, HWND=0x" << std::hex << (uintptr_t)g_gameHwnd;
    }

    ImGui_ImplDX9_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();

    RenderPlayerInfoWidget();

    // NavMesh: tile streaming (~once per second)
    // Initialization is done in dllmain.cpp (outputDir + "mmaps")
    {
        auto& navMesh = nav::NavMesh::Instance();
        static uint64_t lastTileUpdateTick = 0;

        if (game::world::IsInGame()) {
            uint64_t now = GetTickCount64();
            if (now - lastTileUpdateTick > 1000) {
                lastTileUpdateTick = now;

                uint32_t mapId = *reinterpret_cast<uint32_t*>(offsets::globals::MapId);
                navMesh.LoadMap(mapId);

                auto player = game::GetLocalPlayer();
                if (player && navMesh.IsReady()) {
                    auto pos = player->GetPosition();
                    navMesh.UpdateLoadedTiles(pos.x, pos.y);
                }
            }
        }
    }

    // Bot: tick the action queue + render widgets
    bot::ActionQueue::Instance().Tick();
    RenderToolsWidget();
    RenderQueueWidget();
    RenderRadarWidget();

    ImGui::EndFrame();
    ImGui::Render();
    ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData());

    return g_originalEndScene(pDevice);
}

// ---- Reset hook ----

static HRESULT WINAPI HookedReset(IDirect3DDevice9* pDevice, D3DPRESENT_PARAMETERS* pParams)
{
    if (g_imguiInitialized && !g_shutdownRequested)
        ImGui_ImplDX9_InvalidateDeviceObjects();

    HRESULT hr = g_originalReset(pDevice, pParams);

    if (g_imguiInitialized && !g_shutdownRequested && SUCCEEDED(hr))
        ImGui_ImplDX9_CreateDeviceObjects();

    return hr;
}

// ---- vtable discovery via dummy device ----

static bool GetD3D9VtableAddresses(void*& outEndScene, void*& outReset)
{
    // Create dummy window
    WNDCLASSEXA wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = DefWindowProcA;
    wc.hInstance = GetModuleHandleA(nullptr);
    wc.lpszClassName = "ImGuiDummyD3D9";
    RegisterClassExA(&wc);

    HWND hDummy = CreateWindowExA(0, wc.lpszClassName, "", WS_OVERLAPPEDWINDOW,
        0, 0, 100, 100, nullptr, nullptr, wc.hInstance, nullptr);
    if (!hDummy) {
        LOG(ERROR) << "[OVERLAY] Failed to create dummy window";
        UnregisterClassA(wc.lpszClassName, wc.hInstance);
        return false;
    }

    IDirect3D9* pD3D = Direct3DCreate9(D3D_SDK_VERSION);
    if (!pD3D) {
        LOG(ERROR) << "[OVERLAY] Direct3DCreate9 failed";
        DestroyWindow(hDummy);
        UnregisterClassA(wc.lpszClassName, wc.hInstance);
        return false;
    }

    D3DPRESENT_PARAMETERS pp = {};
    pp.Windowed = TRUE;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.hDeviceWindow = hDummy;

    IDirect3DDevice9* pDummyDevice = nullptr;
    HRESULT hr = pD3D->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hDummy,
        D3DCREATE_SOFTWARE_VERTEXPROCESSING, &pp, &pDummyDevice);
    if (FAILED(hr)) {
        LOG(ERROR) << "[OVERLAY] CreateDevice failed: 0x" << std::hex << hr;
        pD3D->Release();
        DestroyWindow(hDummy);
        UnregisterClassA(wc.lpszClassName, wc.hInstance);
        return false;
    }

    // Read vtable
    void** vtable = *(void***)pDummyDevice;
    outEndScene = vtable[42];
    outReset = vtable[16];

    LOG(INFO) << "[OVERLAY] EndScene @ 0x" << std::hex << (uintptr_t)outEndScene;
    LOG(INFO) << "[OVERLAY] Reset @ 0x" << std::hex << (uintptr_t)outReset;

    pDummyDevice->Release();
    pD3D->Release();
    DestroyWindow(hDummy);
    UnregisterClassA(wc.lpszClassName, wc.hInstance);
    return true;
}

// ---- Public API ----

bool Initialize()
{
    // Discover EndScene/Reset addresses via dummy device
    void* endSceneAddr = nullptr;
    void* resetAddr = nullptr;
    if (!GetD3D9VtableAddresses(endSceneAddr, resetAddr))
        return false;

    // Store targets for Shutdown
    g_endSceneTarget = endSceneAddr;
    g_resetTarget = resetAddr;

    // Hook EndScene
    MH_STATUS status = MH_CreateHook(endSceneAddr, &HookedEndScene,
        reinterpret_cast<void**>(&g_originalEndScene));
    if (status != MH_OK) {
        LOG(ERROR) << "[OVERLAY] MH_CreateHook(EndScene) failed: " << MH_StatusToString(status);
        return false;
    }
    status = MH_EnableHook(endSceneAddr);
    if (status != MH_OK) {
        LOG(ERROR) << "[OVERLAY] MH_EnableHook(EndScene) failed: " << MH_StatusToString(status);
        MH_RemoveHook(endSceneAddr);
        return false;
    }

    // Hook Reset
    status = MH_CreateHook(resetAddr, &HookedReset,
        reinterpret_cast<void**>(&g_originalReset));
    if (status != MH_OK) {
        LOG(ERROR) << "[OVERLAY] MH_CreateHook(Reset) failed: " << MH_StatusToString(status);
        MH_DisableHook(endSceneAddr);
        MH_RemoveHook(endSceneAddr);
        return false;
    }
    status = MH_EnableHook(resetAddr);
    if (status != MH_OK) {
        LOG(ERROR) << "[OVERLAY] MH_EnableHook(Reset) failed: " << MH_StatusToString(status);
        MH_RemoveHook(resetAddr);
        MH_DisableHook(endSceneAddr);
        MH_RemoveHook(endSceneAddr);
        return false;
    }

    // Note: HWND discovery and WndProc subclassing are deferred to first EndScene call
    // (the D3D device gives us the correct HWND, and subclassing runs on the right thread)

    LOG(INFO) << "[OVERLAY] Initialized successfully (ImGui deferred to first EndScene)";
    return true;
}

void Shutdown()
{
    g_shutdownRequested = true;
    Sleep(100); // Let in-flight EndScene calls finish

    // Restore original WndProc
    if (g_originalWndProc && g_gameHwnd) {
        SetWindowLongPtrA(g_gameHwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(g_originalWndProc));
        g_originalWndProc = nullptr;
    }

    // Tear down ImGui
    if (g_imguiInitialized) {
        ImGui_ImplDX9_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
        g_imguiInitialized = false;
    }

    // Remove only our MinHook hooks (MH_Uninitialize is called later by hooks::Shutdown)
    if (g_endSceneTarget) {
        MH_DisableHook(g_endSceneTarget);
        MH_RemoveHook(g_endSceneTarget);
        g_endSceneTarget = nullptr;
    }
    if (g_resetTarget) {
        MH_DisableHook(g_resetTarget);
        MH_RemoveHook(g_resetTarget);
        g_resetTarget = nullptr;
    }

    LOG(INFO) << "[OVERLAY] Shut down";
}

} // namespace overlay
