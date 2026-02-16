#pragma once

#include <cstdint>

namespace Wow::Offsets
{
    // ========================================================================
    // 1. GLOBAL POINTERS & MANAGERS (Глобальные указатели)
    // ========================================================================
    namespace Global {
        constexpr uintptr_t ClientConnection        = 0x00C79CE0;   // Базовый указатель на сессию
        constexpr uintptr_t ObjectManagerOffset     = 0x2ED0;       // Смещение от ClientConnection к ObjectMgr
        constexpr uintptr_t GameWindow              = 0x00D41660;   // Хендл окна (ChatboxIsOpen context) 
        constexpr uintptr_t PlayerName              = 0x00C79D18;   // Имя локального игрока 
        constexpr uintptr_t CurrentRealm            = 0x00C79B9E;   // Текущий реалм 
        constexpr uintptr_t GameState               = 0x00B6A9E0;   // Состояние игры (в мире/логин) 
    }

    // ========================================================================
    // 2. OBJECT MANAGER (Менеджер объектов)
    // ========================================================================
    namespace ObjectMgr {
        constexpr uintptr_t FirstObject             = 0xAC;         // Голова связного списка
        constexpr uintptr_t NextObject              = 0x3C;         // Смещение к следующему объекту
        constexpr uintptr_t LocalGuid               = 0xC0;         // GUID локального игрока внутри менеджера
    }

    // ========================================================================
    // 3. WOW OBJECT COMMON (Базовые смещения объектов)
    // ========================================================================
    namespace Object {
        constexpr uintptr_t DescriptorFields        = 0x8;          // Указатель на массив UnitFields
        constexpr uintptr_t TypeID                  = 0x14;         // Тип объекта (Player, Unit, Item...)
        constexpr uintptr_t Guid                    = 0x30;         // Полный GUID объекта (иногда 0x0 в дескрипторе)

        // Координаты объекта (X, Y, Z) и Поворот
        constexpr uintptr_t PosX                    = 0x79C;
        constexpr uintptr_t PosY                    = 0x798;
        constexpr uintptr_t PosZ                    = 0x7A0;
        constexpr uintptr_t Rotation                = 0x7A8;        // Facing
    }

    // ========================================================================
    // 4. UNIT FIELDS (Дескрипторы для Unit/Player)
    // ========================================================================
    // Смещения в массиве DescriptorFields (Index * 4)
    namespace Descriptors {
        // Unit Fields
        constexpr uintptr_t Health                  = 0x18 * 4;     // UNIT_FIELD_HEALTH
        constexpr uintptr_t MaxHealth               = 0x20 * 4;     // UNIT_FIELD_MAXHEALTH
        constexpr uintptr_t Power                   = 0x19 * 4;     // Mana/Rage/Energy (UNIT_FIELD_POWER1)
        constexpr uintptr_t MaxPower                = 0x21 * 4;     // Max Mana...
        constexpr uintptr_t Level                   = 0x36 * 4;     // UNIT_FIELD_LEVEL
        constexpr uintptr_t FactionTemplate         = 0x37 * 4;     // UNIT_FIELD_FACTIONTEMPLATE
        constexpr uintptr_t UnitFlags               = 0x3B * 4;     // UNIT_FIELD_FLAGS (бой, лут и т.д.)
        constexpr uintptr_t TargetGUID              = 0x12 * 4;     // UNIT_FIELD_TARGET
        constexpr uintptr_t ChannelObject           = 0x14 * 4;     // UNIT_FIELD_CHANNEL_OBJECT (GUID кастуемого)
        constexpr uintptr_t SummonedBy              = 0xE * 4;      // UNIT_FIELD_SUMMONEDBY

        // Player Fields (Индексы идут после Unit fields, UnitEnd = 0x94)
        constexpr uintptr_t XP                      = 0x27A * 4;    // PLAYER_XP
        constexpr uintptr_t NextLevelXP             = 0x27B * 4;    // PLAYER_NEXT_LEVEL_XP
        constexpr uintptr_t PlayerFlags             = 0x96 * 4;     // PLAYER_FLAGS
        constexpr uintptr_t Coinage                 = 0x492 * 4;    // Деньги (медь) 
    }

    // ========================================================================
    // 5. FUNCTIONS (Адреса функций)
    // ========================================================================
    namespace Functions {
        constexpr uintptr_t ClickToMove             = 0x00611130;   // CGPlayer_C__ClickToMove
        constexpr uintptr_t Interact                = 0x00527F00;   // lua_InteractUnit (или wrapper) 
        constexpr uintptr_t Spell_C_CastSpell       = 0x0080DA40;   // Spell_C__CastSpell 
        constexpr uintptr_t EnumerateObjects        = 0x004D4B30;   // EnumVisibleObjects 
        constexpr uintptr_t GetActiveCamera         = 0x004F5960;   // CGWorldFrame__GetActiveCamera 
        constexpr uintptr_t TraceLine               = 0x007A3B70;   // Traceline (Raycast) 
        constexpr uintptr_t PerformDefaultAction    = 0x004F7880;   // CGWorldFrame__PerformDefaultAction (Right Click) 
    }

    // ========================================================================
    // 6. LUA ENGINE (Скриптовый движок)
    // ========================================================================
    namespace Lua {
        constexpr uintptr_t Execute                 = 0x00819210;   // FrameScript_Execute (Lua_DoString) 
        constexpr uintptr_t GetText                 = 0x007225E0;   // FrameScript_GetLocalizedText 
        constexpr uintptr_t GetContext              = 0x00D3F78C;   // lua_State (Global Pointer) 
        constexpr uintptr_t Register                = 0x004181B0;   // FrameScript_RegisterFunction 
        constexpr uintptr_t Unregister              = 0x00817FD0;   // FrameScript_UnregisterFunction 
    }

    // ========================================================================
    // 7. CLICK TO MOVE (CTM) & MOVEMENT
    // ========================================================================
    namespace CTM {
        constexpr uintptr_t Base                    = 0x00CA11D0;   // Глобальная структура CTM (иногда D8) 
        constexpr uintptr_t Push                    = Base + 0x1C;  // Action (Type)
        constexpr uintptr_t GUID                    = Base + 0x20;  // Interact GUID
        constexpr uintptr_t DestX                   = Base + 0x8C;  // Destination X 
        constexpr uintptr_t DestY                   = Base + 0x90;  // Destination Y
        constexpr uintptr_t DestZ                   = Base + 0x94;  // Destination Z
        constexpr uintptr_t Distance                = Base + 0xC;   // Distance to stop
    }

    // ========================================================================
    // 8. DIRECTX / RENDERING
    // ========================================================================
    namespace DirectX {
        constexpr uintptr_t DevicePointer           = 0x00C5DF88;   // Указатель на IDirect3DDevice9
        constexpr uintptr_t WorldFrame              = 0x00B7436C;   // CGWorldFrame

        // Индексы VTable (виртуальной таблицы)
        constexpr int EndSceneIndex                 = 42;
        constexpr int ResetIndex                    = 16;
    }

    // ========================================================================
    // 9. CAMERA (Камера)
    // ========================================================================
    namespace Camera {
        // Указатель на камеру получается через вызов GetActiveCamera (0x4F5960) 
        // или чтение offsets ниже:
        constexpr uintptr_t CameraPtr               = 0x00B7436C;   // Указатель на WorldFrame
        constexpr uintptr_t CameraOffset            = 0x7E20;       // Смещение камеры в WorldFrame 

        // Смещения внутри объекта Camera:
        constexpr uintptr_t X                       = 0x8;
        constexpr uintptr_t Y                       = 0xC;
        constexpr uintptr_t Z                       = 0x10;
        constexpr uintptr_t Matrix                  = 0x14;         // View Matrix (обычно)
        constexpr uintptr_t FOV                     = 0x38;
    }
}