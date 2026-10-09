#pragma once

#include <cstddef>
#include <cstdint>

// Layout fragments verified by disassembly. These declarations intentionally
// describe data only; callers still need safe handle resolution and lifetime
// checks before using any resolved reference.
namespace Skyrim::Offsets
{
    namespace TESReferenceList
    {
        // Relative to the TES singleton at kSingletonPointerRva.
        // The entries are 32-bit object-reference handles, not raw pointers.
        inline constexpr std::ptrdiff_t kHandleData = 0x30;
        inline constexpr std::ptrdiff_t kHandleCount = 0x40;
        inline constexpr std::size_t kHandleSize = sizeof(std::uint32_t);

        // sub_14077A100 walks this handle list, resolves every entry, and
        // compares resolved TESObjectREFR positions. Names are behavioral
        // descriptions, not original symbols.
        inline constexpr std::uintptr_t kRangeEnumerationRva = 0x77A100ULL;
    }

    namespace HavokEntityList
    {
        // Separate from TESReferenceList: this is a physics-entity pointer
        // list observed in the Havok serializer at RVA 0xB60780.
        inline constexpr std::ptrdiff_t kEntityData = 0x60;
        inline constexpr std::ptrdiff_t kEntityCount = 0x68;
        inline constexpr std::ptrdiff_t kEntityCapacityAndFlags = 0x6C;
        inline constexpr std::size_t kEntityPointerSize = sizeof(void*);
    }

    namespace ReferenceHandle
    {
        // sub_140261C70 validates and resolves a 32-bit reference handle into
        // a strong TESObjectREFR reference. Prefer this engine function to a
        // hand-rolled table lookup: it owns the required reference-count work.
        inline constexpr std::uintptr_t kResolveRva = 0x261C70ULL;

        // Backing table facts used by the resolver. An entry is 0x10 bytes;
        // its pointer field refers 0x20 bytes past the TESObjectREFR base.
        inline constexpr std::uintptr_t kEntryTableRva = 0x20FDA00ULL;
        inline constexpr std::size_t kEntrySize = 0x10;
        inline constexpr std::ptrdiff_t kEntryFlagsAndSerial = 0x00;
        inline constexpr std::ptrdiff_t kEntryManagedPointer = 0x08;
        inline constexpr std::ptrdiff_t kObjectFromManagedPointer = -0x20;

        inline constexpr std::uint32_t kIndexMask = 0x000F'FFFF;
        inline constexpr std::uint32_t kSerialMask = 0x03F0'0000;
        inline constexpr std::uint32_t kLiveFlag = 0x0400'0000;
        inline constexpr unsigned int kIndexInReferenceStateShift = 11;
        inline constexpr std::ptrdiff_t kReferenceState = 0x28;
    }

    namespace TESObjectREFR
    {
        inline constexpr std::uintptr_t kVtableRva = 0x17AF140ULL;
        inline constexpr std::uintptr_t kActorVtableRva = 0x189EC20ULL;

        // TESObjectREFR and Actor point at sub_1402E55A0 in this virtual slot.
        // PlayerCharacter overrides it at RVA 0x737170 and can return its
        // third-person root; callers should dispatch through the vtable.
        inline constexpr std::size_t kGet3DVirtualSlot = 0x70;
        inline constexpr std::ptrdiff_t kGet3DVirtualOffset = 0x380;
        inline constexpr std::uintptr_t kGet3DRva = 0x2E55A0ULL;

        // Implementation detail of kGet3DRva. Use the function above rather
        // than following this path directly: the function also handles its
        // thread-local fast path.
        inline constexpr std::ptrdiff_t kRuntime3DState = 0x68;
        inline constexpr std::ptrdiff_t kRoot3DInRuntimeState = 0x68;
    }
}
