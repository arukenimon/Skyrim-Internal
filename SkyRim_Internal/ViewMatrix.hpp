#pragma once

#include <cstddef>
#include <cstdint>

// Camera/view-projection layout recovered from SkyrimSE.exe (the attached AE
// database). These are runtime offsets: check that the singleton/root/child is
// non-null before reading, and do not cache scene-graph pointers across loads.
namespace Skyrim::Offsets
{
    namespace PlayerCamera
    {
        // Pointer-to-PlayerCamera singleton. Dereference moduleBase + this RVA.
        inline constexpr std::uintptr_t kSingletonPointerRva = 0x30FEBF8ULL;

        // TESCamera base field. It is the scene-graph root used by camera
        // implementations; locate the active NiCamera below this root.
        inline constexpr std::ptrdiff_t kCameraRoot = 0x20;
    }

    namespace NiNode
    {
        // NiTObjectArray<NiPointer<NiAVObject>>. The attached camera is a
        // child of the camera root; select the child whose vtable is NiCamera.
        inline constexpr std::ptrdiff_t kChildrenData = 0x118; // NiAVObject**
        inline constexpr std::ptrdiff_t kChildrenCount = 0x120; // std::uint16_t
    }

    namespace NiCamera
    {
        inline constexpr std::size_t kSize = 0x188;
        inline constexpr std::uintptr_t kVtableRva = 0x19AE468ULL;

        // float[16], recomputed when the camera's world data changes. Despite
        // the common "view matrix" name, this is the combined view-projection
        // matrix suitable for world-to-screen projection.
        inline constexpr std::ptrdiff_t kViewProjectionMatrix = 0x110;
        // Row-major storage, multiplied by a column world point. Verified by
        // the engine's WorldPtToScreenPt routine at RVA 0xD2DE10. The W row
        // is +0x140/+0x144/+0x148/+0x14C; never transpose this matrix.
        inline constexpr std::size_t kViewProjectionFloatCount = 16;
        inline constexpr std::size_t kViewProjectionByteSize =
            kViewProjectionFloatCount * sizeof(float);

        // NiFrustum starts here. The engine routine below builds the combined
        // matrix from this frustum and NiAVObject::kWorldTransform.
        inline constexpr std::ptrdiff_t kFrustum = 0x150;
        inline constexpr std::uintptr_t kUpdateViewProjectionRva = 0xD2E330ULL;
        inline constexpr std::uintptr_t kWorldToScreenRva = 0xD2DE10ULL;
        // NiRect<float>, normalized to the backbuffer with Y increasing up.
        inline constexpr std::ptrdiff_t kViewportLeft = 0x174;
        inline constexpr std::ptrdiff_t kViewportRight = 0x178;
        inline constexpr std::ptrdiff_t kViewportTop = 0x17C;
        inline constexpr std::ptrdiff_t kViewportBottom = 0x180;
    }
}
