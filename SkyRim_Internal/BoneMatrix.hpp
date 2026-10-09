#pragma once

#include <cstddef>
#include <cstdint>

// Bone and world-transform layout recovered from NiSkinInstance update code.
// The transform pointers are live scene-graph data; obtain them only while the
// actor's 3D is loaded and validate every pointer before use.
namespace Skyrim::Offsets
{
    namespace NiAVObject
    {
        // Non-owning NiNode* parent. The setter at RVA 0xD1DF50 reads the
        // old parent from +0x30, detaches this child, then writes the new one.
        // +0x40 is the owned collision object, not the scene parent.
        inline constexpr std::ptrdiff_t kParent = 0x30;

        // NiTransform objects are 0x34 bytes: 3x3 rotation, translation, scale.
        inline constexpr std::ptrdiff_t kLocalTransform = 0x48;
        inline constexpr std::ptrdiff_t kWorldTransform = 0x7C;

        // Relative to kWorldTransform. These three floats are the world-space
        // point to project for a bone overlay.
        inline constexpr std::ptrdiff_t kWorldTranslation = 0x24;
        inline constexpr std::ptrdiff_t kWorldPositionX = kWorldTransform + kWorldTranslation;
        inline constexpr std::ptrdiff_t kWorldPositionY = kWorldPositionX + sizeof(float);
        inline constexpr std::ptrdiff_t kWorldPositionZ = kWorldPositionY + sizeof(float);
    }

    namespace NiSkinInstance
    {
        inline constexpr std::uintptr_t kVtableRva = 0x19AF490ULL;
        inline constexpr std::uintptr_t kRttiTypeDescriptorRva = 0x20C5288ULL;

        inline constexpr std::ptrdiff_t kSkinData = 0x10;
        inline constexpr std::ptrdiff_t kBoneNodes = 0x28;           // NiNode**
        inline constexpr std::ptrdiff_t kBoneWorldTransforms = 0x30; // NiTransform**

        // Relative to the NiSkinData pointer. This count indexes both arrays.
        inline constexpr std::ptrdiff_t kBoneCountInSkinData = 0x58;
    }

    namespace NiGeometry
    {
        inline constexpr std::uintptr_t kVtableRva = 0x19B1ED8ULL;
        inline constexpr std::uintptr_t kRttiTypeDescriptorRva = 0x20C5540ULL;

        // NiGeometry-derived objects retain these two pointers at the base
        // layout. The latter is NiSkinInstance (or a derived skin instance).
        inline constexpr std::ptrdiff_t kGeometryData = 0x120;
        inline constexpr std::ptrdiff_t kSkinInstance = 0x128;
    }

    namespace BSTriShape
    {
        // BSGeometry owns the NiSkinInstance at +0x130. Verified in its
        // destructor at RVA 0xD395A0 and bound update at 0xD397E0, which
        // passes this pointer to NiSkinInstance's update at 0xD46C20.
        inline constexpr std::ptrdiff_t kSkinInstance = 0x130;
    }

    namespace NiTransform
    {
        inline constexpr std::size_t kSize = 0x34;
        inline constexpr std::ptrdiff_t kRotation = 0x00;    // 3x3 float matrix
        inline constexpr std::ptrdiff_t kTranslation = 0x24; // float[3]
        inline constexpr std::ptrdiff_t kScale = 0x30;
    }
}
