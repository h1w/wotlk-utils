# Static Addresses & Globals — WoW 3.3.5a (build 12340)

> Source: offsets.txt, various sections (camera, collision, console, globals, etc.)

## Camera / Spectator Mode

| Address/Offset | Name | Notes |
|----------------|------|-------|
| `0x004F5960` | CGWorldFrame__GetActiveCamera | Function |
| `0x00B7436C` | Camera_Pointer | |
| `0x7E20` | Camera_Offset | |
| `0x8` | Camera_X | Offset from camera base |
| `0xC` | Camera_Y | Offset from camera base |
| `0x10` | Camera_Z | Offset from camera base |
| `0x88` | Camera_Follow_GUID | Offset from camera base |
| `0x1008` | Spectator_Player_Base | + PlayerBase |
| `0x0A` | Spectator_Player_IsSpectating | + [PlayerBase + Spectator_Player_Base]. byte => 255 => IsCommentator |
| `0xACE4A8` | Spectator_Base | |
| `0x0C` | Spectator_X | + Spectator_Base |
| `0x10` | Spectator_Y | + Spectator_Base |
| `0x14` | Spectator_Z | + Spectator_Base |
| `0x20` | Spectator_FollowGUID | + Spectator_Base. Set to own GUID |
| `0x40` | Spectator_CamZoom | + Spectator_Base |
| `0x44` | Spectator_CamSpeed | + Spectator_Base |
| `0x48` | Spectator_Collision | + Spectator_Base |

## Collision

| Address | Name | Notes |
|---------|------|-------|
| `0x007A524D` | M2Collision1 | objects |
| `0x007A50CF` | M2Collision2 | objects |
| `0x007AE7EA` | WMOCollision | buildings |
| `0x007D889B` | ADTCollision | terrain |

## Console / CVar

| Address | Name | Notes |
|---------|------|-------|
| `0x00CABCC4` | Console Active | 1 = active, 0 = inactive |
| `0x00ADBAC4` | Console Key | DirectInput Keycode |
| `0x00CA1978` | Console Open | 1 = open, 0 = closed |
| `0x00765360` | Console WriteA | Function |
| `0x00769100` | Console RegisterCommand | Function |
| `0x007689E0` | Console UnregisterCommand | Function |
| `0x00C5DF7C` | CVar_MaxFPS | |

## Globals

| Address | Name | Notes |
|---------|------|-------|
| `0x00C79D18` | PlayerName | |
| `0x00B6AA40` | CurrentAccount | |
| `0x00C79B9E` | CurrentRealm | |
| `0x00BD07B0` | CurrentTargetGUID | |
| `0x00BD07B8` | LastTargetGUID | |
| `0x00BD07A0` | MouseOverGUID | |
| `0x00CA11F8` | FollowGUID | |
| `0x00BD084D` | ComboPoint | |
| `0x00BFA8D8` | LootWindow | |
| `0x00BE5D88` | KnownSpell | |
| `0x00B6AA38` | IsLoadingOrConnecting | |
| `0xD8` | Movement_Field | Offset |
| `0x00D3F5AC` | SpellCooldownPtr | |
| `0x00B1D618` | Timestamp | |
| `0x00B499A4` | LastHardwareAction | |
| `0x004D4DB0` | ClntObjMgrObjectPtr | Function |
| `0x004D3790` | ClntObjMgrGetActivePlayer | Function |
| `0x004038F0` | ClntObjMgrGetActivePlayerObj | Function |
| `0x00527830` | HandleTerrainClick | Function |
| `0x00524BF0` | CGGameUI_Target | Function |
| `0x0080DA40` | Spell_C_CastSpell | Function |
| `0x0071F300` | CGUnit_C__GetCreatureType | Function |
| `0x964` | UnitName1 | Offset |
| `0x5C` | UnitName2 | Offset |
| `0x00BFA3F0` | nbItemsSellByMerchant | |
| `0x00C24954` | CInputControl | |
| `0x00B3203C` | BuildNumber | |
| `0x00BD077C` | GetMinimapZoneText | |
| `0x00BD0788` | GetZoneText | |
| `0x00BD0784` | GetSubZoneText | |
| `0x00CE06D0` | GetInternalMapName | |
| `0x00CA1238` | LocalGUID | |
| `0x00BD080C` | GetZoneID | |
| `0xBC` | IsBobbingOffset | |
| `0x00D41660` | ChatboxIsOpen | |
| `0x0077FBF0` | M2Model__IsOutdoors | Function |
| `0x004FAF90` | CGWorldFrame__RenderWorld | Function |
| `0x0077F310` | CGWorldFrame__Intersect | Function |

## Battleground

| Address | Name | Notes |
|---------|------|-------|
| `0x00BEA588` | IsBattlegroundFinished | |

## CGUnit_C__GetCreatureRank

| Address | Name | Notes |
|---------|------|-------|
| `0x00718DE0` | CGUnit_C__GetCreatureRank | Function |
| `0x964` | Offset1 | |
| `0x18` | Offset4 | |

## ShapeshiftForm

| Address | Name | Notes |
|---------|------|-------|
| `0x0071AF70` | CGUnit_C__GetShapeshiftFormId | Function |
| `0xD0` | BaseAddress_Offset1 | |
| `0x1D3` | BaseAddress_Offset2 | |

## Lua

| Address | Name | Notes |
|---------|------|-------|
| `0x00D3F78C` | Lua_State | |
| `0x00819210` | Lua_DoString (FrameScript_Execute) | Function |
| `0x007225E0` | Lua_GetLocalizedText | Function |
| `0x0084DBF0` | Lua_SetTop | Function |
| `0x0084F860` | LuaLoadBuffer | Function |
| `0x0084EC50` | LuaPCall | Function |
| `0x0084DBD0` | LuaGetTop | Function |
| `0x0084DEB0` | LuaType | Function |
| `0x0084E030` | LuaToNumber | Function |
| `0x0084E0E0` | LuaToLString | Function |
| `0x0084E0B0` | LuaToBoolean | Function |

## Movements

| Address | Name | Notes |
|---------|------|-------|
| `0x005FC200` | MoveForwardStart | Function |
| `0x005FC250` | MoveForwardStop | Function |
| `0x005FC290` | MoveBackwardStart | Function |
| `0x005FC2E0` | MoveBackwardStop | Function |
| `0x005FC320` | TurnLeftStart | Function |
| `0x005FC360` | TurnLeftStop | Function |
| `0x005FC3B0` | TurnRightStart | Function |
| `0x005FC3F0` | TurnRightStop | Function |
| `0x005FBF80` | JumpOrAscendStart | Function |
| `0x005FC0A0` | AscendStop | Function |

## ObjectManager

| Address/Offset | Name | Notes |
|----------------|------|-------|
| `0x00C79CE0` | CurMgrPointer | |
| `0x2ED0` | CurMgrOffset | |
| `0x3C` | NextObject | Offset |
| `0xAC` | FirstObject | Offset |
| `0xC0` | LocalGUID | Offset |
| `0x004D4B30` | EnumVisibleObjects | Function |
| `0x004D4DB0` | GetObjectByGuid | Function |
| `0x004D3790` | GetLocalPlayerGuid | Function |

## Object

| Address | Name | Notes |
|---------|------|-------|
| `54` | GetObjectName | VTable index |
| `12` | GetObjectLocation | VTable index |
| `14` | GetObjectFacing | VTable index |
| `44` | Interact | VTable index |
| `0x00524BF0` | SelectObject | Function |

## Item

| Address | Name | Notes |
|---------|------|-------|
| `0x00708C20` | UseItem | Function |
| `0x006DC3F0` | CanUseItem | Function |

## Container

| Address | Name | Notes |
|---------|------|-------|
| `0x005D6F20` | GetBagAtIndex | Function |
| `0x00BFA8D8` | LootWindowOffset | |

## Unit

| Address | Name | Notes |
|---------|------|-------|
| `0xD70` | FishChanneledCasting | Offset |
| `0xC20` | ChanneledCastingId | Offset |
| `0xC08` | CastingId | Offset |
| `0x0073E410` | UpdateDisplayInfo | Function (TODO: IMPLEMENT) |
| `0x007251C0` | UnitReaction | Function |
| `0x007282A0` | HasAuraBySpellId | Function |
| `0x00556E10` | GetAura | Function |
| `0x004F8850` | GetAuraCount | Function |
| `0x0071F300` | GetCreatureType | Function |
| `0x00718A00` | GetCreatureRank | Function |
| `0x0071AF70` | ShapeshiftFormId | Function |
| `0x007374C0` | CalculateThreat | Function |

## LocalPlayer

| Address | Name | Notes |
|---------|------|-------|
| `0x00727400` | ClickToMove (CGPlayer_C__ClickToMove) | Function |
| `0x0072EA50` | SetFacing | Function |
| `0x00721F90` | IsClickMoving | Function |
| `0x0072B3A0` | StopCTM | Function |
| `0x0051F430` | CorpsePosition | Function |
| `0x00BD084D` | ComboPoints | |
| `0x00ACFDF4` | CompletedQuests | |
| `0xC24388` | RuneState | |
| `0xC24304` | RuneType | |
| `0xC24364` | RuneCooldown | |

## Corpse

| Address | Name | Notes |
|---------|------|-------|
| `0x00BD0A58` | X | |
| `0x00BD0A5C` | Y | X + 0x4 |
| `0x00BD0A60` | Z | X + 0x8 |

## Party

| Address | Name | Notes |
|---------|------|-------|
| `0x00BD1968` | s_LeaderGUID | |
| `0x00BD1948` | s_Member1GUID | |
| `0x00BD1950` | s_Member2GUID | s_Member1GUID + 0x8 |
| `0x00BD1958` | s_Member3GUID | s_Member2GUID + 0x8 |
| `0x00BD1960` | s_Member4GUID | s_Member3GUID + 0x8 |
| `0x00BD1948` | PartyArray | |

## Raid

| Address | Name | Notes |
|---------|------|-------|
| `0x00C543E0` | RaidCount | |
| `0x00C54340` | RaidArray | |
| `0x00C4EC2C` | InstanceDifficulty | |

## Spell

| Address | Name | Notes |
|---------|------|-------|
| `0x00BE8D9C` | SpellCount | |
| `0x00BE5D88` | SpellBook | |
| `0x0080DA40` | CastSpell | Function |
| `0x00807980` | GetSpellCooldown | Function |
| `0xC1E358` | FirstActionBarSpellId | |

## World

| Address | Name | Notes |
|---------|------|-------|
| `0x007A3B70` | Traceline | Function |
| `0x00AB63BC` | CurrentMapId | |
| `0x00CE06D0` | InternalMapName | |
| `0x00BD080C` | ZoneID | |
| `0x00BD0788` | ZoneText | |
| `0x00BD0784` | SubZoneText | |

## Events

| Address | Name | Notes |
|---------|------|-------|
| `0x004DDBD0` | EventVictim | Function |

## DBC (Database Client Functions)

| Address | Name | Notes |
|---------|------|-------|
| `0x006337D0` | RegisterBase | Function |
| `0x004BB1C0` | GetRow | Function |
| `0x004CFD20` | GetLocalizedRow | Function |

## WDB (World Database Cache)

| Address | Name | Notes |
|---------|------|-------|
| `0x0067FA80` | DbWoWCache_GetInfoBlockById | Function |
| `0x0067CA30` | DdItemCache_GetInfoBlockByID | Function |
| `0x0067DE90` | DbQuestCache_GetInfoBlockByID | Function |
| `0x00C5D828` | WdbItemCache | |
| `0x00C5DA48` | WdbQuestCache | |

## Drawing / Rendering

| Address | Name | Notes |
|---------|------|-------|
| `0x00B7436C` | WorldFrame | |
| `0x7E20` | ActiveCamera | Offset |
| `0x002532E0` | RenderBackground | Function (UPDATE) |

## Direct3D9

| Address | Name | Notes |
|---------|------|-------|
| `0x00C5DF88` | pDevicePtr_1 | |
| `0x397C` | pDevicePtr_2 | Offset |
| `0xA4` | oBeginScene | Offset |
| `0xA8` | oEndScene | Offset |
| `0xAC` | oClear | Offset |

## ClickToMove (CTM)

| Address/Offset | Name | Notes |
|----------------|------|-------|
| `0x00727400` | CGPlayer_C__ClickToMove | Function |
| `0xBD08F4` | CTM_Activate_Pointer | |
| `0x30` | CTM_Activate_Offset | |
| `0x00CA11D8` | CTM_Base | |
| `0x8C` | CTM_X | + CTM_Base |
| `0x90` | CTM_Y | + CTM_Base |
| `0x94` | CTM_Z | + CTM_Base |
| `0x4` | CTM_TurnSpeed | + CTM_Base |
| `0xC` | CTM_Distance | + CTM_Base |
| `0x1C` | CTM_Action | + CTM_Base |
| `0x20` | CTM_GUID | + CTM_Base |

## IsFlying

Reversed from Lua_IsFlying

| Offset | Name | Value |
|--------|------|-------|
| `0x44` | IsFlyingOffset | |
| `0x2000000` | IsFlying_Mask | |

## IsSwimming

Reversed from Lua_IsSwimming

| Offset | Name | Value |
|--------|------|-------|
| `0xA30` | IsSwimmingOffset | |
| `0x200000` | IsSwimming_Mask | |

## AutoLoot

| Address | Name | Notes |
|---------|------|-------|
| `0x00BD0914` | AutoLoot_Activate_Pointer | |
| `0x30` | AutoLoot_Activate_Offset | |

## AutoSelfCast

| Address | Name | Notes |
|---------|------|-------|
| `0xBD0920` | AutoSelfCast_Activate_Pointer | |
| `0x30` | AutoSelfCast_Activate_Offset | |

## WoWChat

| Address | Name | Notes |
|---------|------|-------|
| `0x00B75A60` | ChatBufferStart | |
| `0x17C0` | NextMessage | Offset |

## UnitBaseGetUnitAura

| Address | Name | Notes |
|---------|------|-------|
| `0x00556E10` | CGUnit_Aura | Function |
| `0xDD0` | AURA_COUNT_1 | Offset |
| `0xC54` | AURA_COUNT_2 | Offset |
| `0xC50` | AURA_TABLE_1 | Offset |
| `0xC58` | AURA_TABLE_2 | Offset |
| `0x18` | AURA_SIZE | |
| `0x8` | AURA_SPELL_ID | Offset |

## Other

| Address | Name | Notes |
|---------|------|-------|
| `0x0086AE20` | PerformanceCounter | Function |
| `0x00B499A4` | LastHardwareAction | |
| `0xBC` | IsBobbing | Offset |
| `0x00BEBA40` | WorldLoaded | TODO: IMPLEMENT |
| `0x00B6A9E0` | GameState | |
| `0x00B6AA38` | IsLoading | |
| `0x00C79B9E` | RealmName | |
| `0xC0F448` | AHListAuctions | TODO: IMPLEMENT |
| `0xC0F444` | AHListNumAuctions | TODO: IMPLEMENT |
| `0xC0F408` | AHListTotalAuctions | TODO: IMPLEMENT |
| `0x00A37F0C` | wallclimb angle | |
| `0x0072B52C` | FollowNpc's | |
| `0x0052B25F` | DisableAFK | |
