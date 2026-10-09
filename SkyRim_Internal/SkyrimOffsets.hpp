#pragma once

#include <cstddef>
#include <cstdint>

// Binary-specific constants recovered from SkyrimSE.exe.i64.
//
// Values named *Rva are relative to kImageBase. Revalidate all of them after
// updating SkyrimSE.exe; an RVA is not guaranteed to survive a game update.
namespace Skyrim::Offsets
{
    inline constexpr std::uintptr_t kImageBase = 0x140000000ULL;

    namespace TES
    {
        // VA 0x1420F7DB0. Points to the process-wide TES instance.
        inline constexpr std::uintptr_t kSingletonPointerRva = 0x20F7DB0ULL;
    }

    namespace ObjectReference
    {
        // float fields relative to a resolved TESObjectREFR / Actor instance.
        inline constexpr std::ptrdiff_t kPositionX = 0x54;
        inline constexpr std::ptrdiff_t kPositionY = 0x58;
        inline constexpr std::ptrdiff_t kPositionZ = 0x5C;
    }

    namespace PapyrusObjectReference
    {
        // GetPositionX/Y/Z handlers, registered by the ObjectReference native
        // command table at RVA 0xA33590.
        inline constexpr std::uintptr_t kGetPositionXHandlerRva = 0xA2EDA0ULL;
        inline constexpr std::uintptr_t kGetPositionYHandlerRva = 0xA2EDB0ULL;
        inline constexpr std::uintptr_t kGetPositionZHandlerRva = 0xA2EDC0ULL;
    }
}
