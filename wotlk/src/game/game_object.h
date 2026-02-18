#pragma once
// =============================================================================
// WowObject -- lightweight wrapper around a raw object pointer.
//
// Does NOT own the pointer. Valid only during the current frame
// (ObjectManager can relocate between frames).
// =============================================================================

#include "types.h"
#include <cstdint>
#include <string>

namespace game {

class WowObject {
public:
    explicit WowObject(uintptr_t ptr) : m_ptr(ptr) {}

    bool IsValid() const { return m_ptr != 0; }
    uintptr_t Ptr() const { return m_ptr; }

    // --- Descriptors ---
    GUID       GetGUID() const;
    ObjectType GetType() const;
    uint32_t   GetEntry() const;
    float      GetScale() const;

    // --- Descriptor access ---
    uint32_t GetDescU32(int field) const;
    uint64_t GetDescU64(int field) const;
    float    GetDescFloat(int field) const;

    // --- Spatial ---
    Vec3  GetPosition() const;
    float GetFacing() const;
    float DistanceTo(const WowObject& other) const;

    // --- Identity ---
    std::string GetName() const;

protected:
    uintptr_t m_ptr;

    uintptr_t GetDescriptorBase() const;
    uintptr_t GetVTable() const;
};

} // namespace game
