#pragma once
// =============================================================================
// ObjectManager traversal -- enumerating game objects from the linked list.
// =============================================================================

#include "types.h"
#include <functional>

namespace game::objmgr {

// Get the ObjectManager base pointer (CurMgrPointer -> +ObjMgrOffset).
// Returns 0 if not available (not in game).
uintptr_t GetManagerBase();

// True if the ObjectManager pointer chain is valid (i.e., we're in-game).
bool IsInGame();

// Get the local player's GUID from the ObjectManager.
GUID GetLocalPlayerGUID();

// Get a raw object pointer by GUID (calls ClntObjMgrObjectPtr).
// Returns 0 if not found.
uintptr_t GetObjectPtr(GUID guid);

// Get the local player object pointer.
uintptr_t GetLocalPlayerPtr();

// Callback signature for EnumObjects: receives object base pointer.
// Return true to continue enumeration, false to stop.
using EnumCallback = std::function<bool(uintptr_t objPtr)>;

// Walk the ObjectManager linked list, calling cb for each object.
void EnumObjects(const EnumCallback& cb);

} // namespace game::objmgr
