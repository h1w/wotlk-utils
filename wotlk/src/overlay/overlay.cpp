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
#include "../game/player_stats.h"

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
