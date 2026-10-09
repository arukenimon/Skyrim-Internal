#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

#include "ActorState.hpp"
#include "BoneMatrix.hpp"
#include "EntityList.hpp"
#include "SkyrimOffsets.hpp"
#include "ViewMatrix.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

// Runtime helpers for the verified reference -> loaded 3D -> skin -> bone
// chain. Call them only from a point where the game permits scene-graph access.
// None of the returned scene pointers may be cached across a load, unload, or
// frame boundary.
namespace Skyrim::Runtime
{
    static_assert(sizeof(void*) == 8, "SkyrimSE.exe offsets require an x64 build.");

    using ResolveReferenceHandleFn = bool(__fastcall*)(const std::uint32_t* handle, void** outReference);
    using Get3DFn = void* (__fastcall*)(void* reference);

    [[nodiscard]] inline void* GetTESSingleton(const std::uintptr_t moduleBase)
    {
        const auto singleton = reinterpret_cast<void* const*>(
            moduleBase + Offsets::TES::kSingletonPointerRva);
        return *singleton;
    }

    class ResolvedReference final
    {
    public:
        ResolvedReference() = default;

        ResolvedReference(const std::uintptr_t moduleBase, const std::uint32_t handle)
        {
            static_cast<void>(Resolve(moduleBase, handle));
        }

        ResolvedReference(const ResolvedReference&) = delete;
        ResolvedReference& operator=(const ResolvedReference&) = delete;

        ResolvedReference(ResolvedReference&& other) noexcept
            : moduleBase_(std::exchange(other.moduleBase_, 0)),
              reference_(std::exchange(other.reference_, nullptr))
        {
        }

        ResolvedReference& operator=(ResolvedReference&& other) noexcept
        {
            if (this != &other)
            {
                Reset();
                moduleBase_ = std::exchange(other.moduleBase_, 0);
                reference_ = std::exchange(other.reference_, nullptr);
            }

            return *this;
        }

        ~ResolvedReference()
        {
            Reset();
        }

        [[nodiscard]] bool Resolve(const std::uintptr_t moduleBase, const std::uint32_t handle)
        {
            Reset();
            if (moduleBase == 0 || handle == 0)
                return false;

            moduleBase_ = moduleBase;
            const auto resolve = reinterpret_cast<ResolveReferenceHandleFn>(
                moduleBase + Offsets::ReferenceHandle::kResolveRva);
            return resolve(&handle, &reference_) && reference_ != nullptr;
        }

        void Reset() noexcept
        {
            if (moduleBase_ != 0 && reference_ != nullptr)
            {
                // Passing a null handle follows the resolver's clear path and
                // releases the strong reference it created for this object.
                const std::uint32_t nullHandle = 0;
                const auto resolve = reinterpret_cast<ResolveReferenceHandleFn>(
                    moduleBase_ + Offsets::ReferenceHandle::kResolveRva);
                resolve(&nullHandle, &reference_);
            }

            reference_ = nullptr;
            moduleBase_ = 0;
        }

        [[nodiscard]] void* Get() const noexcept
        {
            return reference_;
        }

        [[nodiscard]] explicit operator bool() const noexcept
        {
            return reference_ != nullptr;
        }

    private:
        std::uintptr_t moduleBase_ = 0;
        void* reference_ = nullptr;
    };

    [[nodiscard]] inline void* GetLoaded3D(const std::uintptr_t moduleBase, void* reference)
    {
        if (moduleBase == 0 || reference == nullptr)
            return nullptr;

        // PlayerCharacter overrides this virtual slot to return its third
        // person 3D when appropriate. Calling the TESObjectREFR base address
        // directly skips that override and misses the visible player body.
        const auto vtable = *reinterpret_cast<std::uintptr_t const*>(reference);
        if (vtable == 0)
            return nullptr;
        const auto get3D = *reinterpret_cast<Get3DFn const*>(
            vtable + Offsets::TESObjectREFR::kGet3DVirtualOffset);
        if (get3D == nullptr)
            return nullptr;
        return get3D(reference);
    }

    namespace Detail
    {
        template <class T>
        [[nodiscard]] inline T ReadAt(const void* base, const std::ptrdiff_t offset)
        {
            const auto address = reinterpret_cast<const std::byte*>(base) + offset;
            return *reinterpret_cast<const T*>(address);
        }

        // MSVC x64 RTTI structures. The binary retains RTTI for the Ni scene
        // classes, allowing derived node/geometry/skin types to be recognized
        // without a fragile list of every derived vtable.
        struct CompleteObjectLocator
        {
            std::uint32_t signature;
            std::uint32_t offset;
            std::uint32_t cdOffset;
            std::uint32_t typeDescriptorRva;
            std::uint32_t classHierarchyDescriptorRva;
            std::uint32_t selfRva;
        };

        struct ClassHierarchyDescriptor
        {
            std::uint32_t signature;
            std::uint32_t attributes;
            std::uint32_t baseClassCount;
            std::uint32_t baseClassArrayRva;
        };

        struct BaseClassDescriptor
        {
            std::uint32_t typeDescriptorRva;
            std::uint32_t numContainedBases;
            std::int32_t memberDisplacement;
            std::int32_t vbtableDisplacement;
            std::int32_t vbaseDisplacement;
            std::uint32_t attributes;
            std::uint32_t classHierarchyDescriptorRva;
        };

        static_assert(sizeof(CompleteObjectLocator) == 0x18);
        static_assert(sizeof(ClassHierarchyDescriptor) == 0x10);
        static_assert(sizeof(BaseClassDescriptor) == 0x1C);

        // Reuse VirtualQuery results only within one render callback. This
        // avoids a system call for every bone/RTTI member, while the next
        // callback starts fresh after potential loads or memory changes.
        class ScopedReadabilityCache final
        {
            struct Region { std::uintptr_t begin, end; bool readable; };
            std::array<Region, 64> regions_{};
            std::size_t count_ = 0;
            std::size_t replacement_ = 0;
            std::size_t queries_ = 0;
            ScopedReadabilityCache* previous_ = nullptr;
            static inline thread_local ScopedReadabilityCache* current_ = nullptr;
        public:
            ScopedReadabilityCache() : previous_(current_) { current_ = this; }
            ~ScopedReadabilityCache() { current_ = previous_; }
            ScopedReadabilityCache(const ScopedReadabilityCache&) = delete;
            ScopedReadabilityCache& operator=(const ScopedReadabilityCache&) = delete;
            [[nodiscard]] std::size_t QueryCount() const { return queries_; }

            [[nodiscard]] static bool Check(const void* address, const std::size_t bytes)
            {
                if (address == nullptr || bytes == 0)
                    return false;
                const auto start = reinterpret_cast<std::uintptr_t>(address);
                if (current_ != nullptr)
                {
                    for (std::size_t index = 0; index < current_->count_; ++index)
                    {
                        const Region& region = current_->regions_[index];
                        if (start >= region.begin && start < region.end)
                            return region.readable && bytes <= region.end - start;
                    }
                }
                MEMORY_BASIC_INFORMATION information{};
                if (current_ != nullptr)
                    ++current_->queries_;
                if (VirtualQuery(address, &information, sizeof(information)) != sizeof(information))
                    return false;
                constexpr DWORD readablePages = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY |
                    PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
                const Region region{
                    reinterpret_cast<std::uintptr_t>(information.BaseAddress),
                    reinterpret_cast<std::uintptr_t>(information.BaseAddress) + information.RegionSize,
                    information.State == MEM_COMMIT && (information.Protect & readablePages) != 0 &&
                        (information.Protect & (PAGE_GUARD | PAGE_NOACCESS)) == 0,
                };
                if (current_ != nullptr)
                {
                    const auto index = current_->count_ < current_->regions_.size()
                        ? current_->count_++ : current_->replacement_++ % current_->regions_.size();
                    current_->regions_[index] = region;
                }
                return region.readable && start >= region.begin && start < region.end && bytes <= region.end - start;
            }
        };

        [[nodiscard]] inline bool IsReadable(const void* address, const std::size_t bytes)
        {
            return ScopedReadabilityCache::Check(address, bytes);
        }

        // Caller must hold a resolved Actor reference for the read. These
        // are two cheap field reads, not health queries or faction scans.
        [[nodiscard]] inline bool TryReadActorVisualState(const void* actor, Actors::VisualState& state)
        {
            state = {};
            if (actor == nullptr)
                return false;
            const auto fields = reinterpret_cast<const std::byte*>(actor) + Offsets::Actor::kLifeStateFlags;
            constexpr auto bytes = static_cast<std::size_t>(
                Offsets::Actor::kActorFlags - Offsets::Actor::kLifeStateFlags) + sizeof(std::uint32_t);
            if (!IsReadable(fields, bytes))
                return false;
            state = Actors::DecodeVisualState(
                ReadAt<std::uint32_t>(actor, Offsets::Actor::kLifeStateFlags),
                ReadAt<std::uint32_t>(actor, Offsets::Actor::kActorFlags));
            return true;
        }

        [[nodiscard]] inline bool TryReadSceneParent(const void* node, void*& parent)
        {
            parent = nullptr;
            if (node == nullptr)
                return false;
            const auto address = reinterpret_cast<const std::byte*>(node) + Offsets::NiAVObject::kParent;
            if (!IsReadable(address, sizeof(void*)))
                return false;
            parent = ReadAt<void*>(node, Offsets::NiAVObject::kParent);
            return true;
        }

        [[nodiscard]] inline const CompleteObjectLocator* GetLocator(
            const std::uintptr_t moduleBase,
            const void* object)
        {
            if (moduleBase == 0 || !IsReadable(object, sizeof(void*)))
                return nullptr;

            const auto vtable = ReadAt<std::uintptr_t>(object, 0);
            if (vtable < sizeof(void*) || !IsReadable(
                    reinterpret_cast<const void*>(vtable - sizeof(void*)),
                    sizeof(CompleteObjectLocator)))
            {
                return nullptr;
            }

            const auto locator = ReadAt<const CompleteObjectLocator*>(
                reinterpret_cast<const void*>(vtable - sizeof(void*)), 0);
            if (locator == nullptr || !IsReadable(locator, sizeof(CompleteObjectLocator)) || locator->signature != 1)
                return nullptr;

            if (locator->selfRva !=
                static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(locator) - moduleBase))
            {
                return nullptr;
            }

            return locator;
        }

        [[nodiscard]] inline bool DerivesFromUncached(
            const std::uintptr_t moduleBase,
            const void* object,
            const std::uint32_t soughtTypeDescriptorRva)
        {
            if (moduleBase == 0 || object == nullptr || soughtTypeDescriptorRva == 0)
                return false;

            const auto locator = GetLocator(moduleBase, object);
            if (locator == nullptr)
                return false;

            const auto hierarchy = reinterpret_cast<const ClassHierarchyDescriptor*>(
                moduleBase + locator->classHierarchyDescriptorRva);
            if (!IsReadable(hierarchy, sizeof(ClassHierarchyDescriptor)) ||
                hierarchy->baseClassCount == 0 || hierarchy->baseClassCount > 64)
                return false;

            const auto baseClassArray = reinterpret_cast<const std::uint32_t*>(
                moduleBase + hierarchy->baseClassArrayRva);
            if (!IsReadable(baseClassArray, sizeof(std::uint32_t) * hierarchy->baseClassCount))
                return false;

            for (std::uint32_t index = 0; index < hierarchy->baseClassCount; ++index)
            {
                const auto descriptor = reinterpret_cast<const BaseClassDescriptor*>(
                    moduleBase + baseClassArray[index]);
                if (IsReadable(descriptor, sizeof(BaseClassDescriptor)) &&
                    descriptor->typeDescriptorRva == soughtTypeDescriptorRva)
                    return true;
            }

            return false;
        }

        [[nodiscard]] inline bool HasBaseTypeNameUncached(
            const std::uintptr_t moduleBase,
            const void* object,
            const char* nameFragment)
        {
            const auto locator = GetLocator(moduleBase, object);
            if (locator == nullptr || nameFragment == nullptr)
                return false;

            const auto hierarchy = reinterpret_cast<const ClassHierarchyDescriptor*>(
                moduleBase + locator->classHierarchyDescriptorRva);
            if (!IsReadable(hierarchy, sizeof(ClassHierarchyDescriptor)) ||
                hierarchy->baseClassCount == 0 || hierarchy->baseClassCount > 64)
            {
                return false;
            }

            const auto baseClassArray = reinterpret_cast<const std::uint32_t*>(
                moduleBase + hierarchy->baseClassArrayRva);
            if (!IsReadable(baseClassArray, sizeof(std::uint32_t) * hierarchy->baseClassCount))
                return false;

            // MSVC x64 TypeDescriptor: two pointers followed by the decorated
            // class name, e.g. ".?AVBSTriShape@@".
            constexpr std::ptrdiff_t kTypeDescriptorName = 2 * sizeof(void*);
            for (std::uint32_t index = 0; index < hierarchy->baseClassCount; ++index)
            {
                const auto descriptor = reinterpret_cast<const BaseClassDescriptor*>(
                    moduleBase + baseClassArray[index]);
                if (!IsReadable(descriptor, sizeof(BaseClassDescriptor)))
                    continue;

                const auto typeName = reinterpret_cast<const char*>(
                    moduleBase + descriptor->typeDescriptorRva + kTypeDescriptorName);
                if (IsReadable(typeName, 1) && std::strstr(typeName, nameFragment) != nullptr)
                    return true;
            }

            return false;
        }

        // Vtables and their RTTI live in the executable image. Cache type
        // queries by vtable, never by a heap object address.
        [[nodiscard]] inline bool DerivesFrom(
            const std::uintptr_t moduleBase, const void* object, const std::uint32_t typeRva)
        {
            if (!IsReadable(object, sizeof(void*)))
                return false;
            const auto vtable = ReadAt<std::uintptr_t>(object, 0);
            using Results = std::unordered_map<std::uint32_t, bool>;
            static thread_local std::unordered_map<std::uintptr_t, Results> cache;
            if (cache.size() > 1024)
                cache.clear();
            auto& results = cache[vtable];
            const auto found = results.find(typeRva);
            if (found != results.end())
                return found->second;
            const bool result = DerivesFromUncached(moduleBase, object, typeRva);
            results.emplace(typeRva, result);
            return result;
        }

        [[nodiscard]] inline bool HasBaseTypeName(
            const std::uintptr_t moduleBase, const void* object, const char* fragment)
        {
            if (fragment == nullptr || !IsReadable(object, sizeof(void*)))
                return false;
            const auto vtable = ReadAt<std::uintptr_t>(object, 0);
            using Results = std::unordered_map<std::string, bool>;
            static thread_local std::unordered_map<std::uintptr_t, Results> cache;
            if (cache.size() > 1024)
                cache.clear();
            auto& results = cache[vtable];
            const auto found = results.find(fragment);
            if (found != results.end())
                return found->second;
            const bool result = HasBaseTypeNameUncached(moduleBase, object, fragment);
            results.emplace(fragment, result);
            return result;
        }
    }

    namespace NiTypeDescriptor
    {
        inline constexpr std::uint32_t kNiNodeRva = 0x203CBF8U;
        inline constexpr std::uint32_t kNiGeometryRva = 0x20C5540U;
        inline constexpr std::uint32_t kNiSkinInstanceRva = 0x20C5288U;
    }

    struct SkinBonesView
    {
        void* skinInstance = nullptr;
        void* skinData = nullptr;
        void** boneNodes = nullptr;
        void** boneWorldTransforms = nullptr;
        std::uint32_t boneCount = 0;
    };

    struct BoneView
    {
        std::uint32_t index = 0;
        void* node = nullptr;
        void* worldTransform = nullptr;
    };

    inline constexpr std::uint32_t kMaxBonesPerSkin = 512;
    inline constexpr std::uint16_t kMaxChildrenPerNode = 4096;
    // A character's 3D normally has far fewer than this.  Keeping the bound
    // small prevents a malformed or world-scale scene root from stalling the
    // render hook during diagnostic capture.
    inline constexpr std::size_t kMaxSceneObjectsPerWalk = 512;
    inline constexpr std::uint32_t kMaxReferenceHandles = 1'000'000;

    [[nodiscard]] inline bool TryGetSkinBones(
        const std::uintptr_t moduleBase,
        void* skinInstance,
        SkinBonesView& result)
    {
        result = {};
        if (!Detail::IsReadable(skinInstance, Offsets::NiSkinInstance::kBoneWorldTransforms + sizeof(void*)) ||
            !Detail::DerivesFrom(moduleBase, skinInstance, NiTypeDescriptor::kNiSkinInstanceRva))
            return false;

        void* const skinData = Detail::ReadAt<void*>(skinInstance, Offsets::NiSkinInstance::kSkinData);
        void** const boneNodes = Detail::ReadAt<void**>(skinInstance, Offsets::NiSkinInstance::kBoneNodes);
        void** const boneWorldTransforms = Detail::ReadAt<void**>(
            skinInstance, Offsets::NiSkinInstance::kBoneWorldTransforms);
        if (skinData == nullptr || boneNodes == nullptr ||
            !Detail::IsReadable(skinData, Offsets::NiSkinInstance::kBoneCountInSkinData + sizeof(std::uint32_t)))
            return false;

        const auto boneCount = Detail::ReadAt<std::uint32_t>(
            skinData, Offsets::NiSkinInstance::kBoneCountInSkinData);
        if (boneCount == 0 || boneCount > kMaxBonesPerSkin)
            return false;
        if (!Detail::IsReadable(boneNodes, sizeof(void*) * boneCount))
            return false;

        result = {
            .skinInstance = skinInstance,
            .skinData = skinData,
            .boneNodes = boneNodes,
            .boneWorldTransforms = boneWorldTransforms,
            .boneCount = boneCount,
        };
        return true;
    }

    // Skyrim AE uses BSTriShape-derived leaves for many actor meshes.  They
    // use BSGeometry's skin field, distinct from NiGeometry's +0x128 field.
    [[nodiscard]] inline bool TryGetTriShapeSkinBones(
        const std::uintptr_t moduleBase,
        void* object,
        SkinBonesView& result)
    {
        result = {};
        if (!Detail::HasBaseTypeName(moduleBase, object, "BSTriShape@@"))
            return false;

        const auto slot = reinterpret_cast<const std::byte*>(object) + Offsets::BSTriShape::kSkinInstance;
        if (!Detail::IsReadable(slot, sizeof(void*)))
            return false;
        void* const skin = Detail::ReadAt<void*>(object, Offsets::BSTriShape::kSkinInstance);
        return skin != nullptr && TryGetSkinBones(moduleBase, skin, result);
    }

    template <class Visitor>
    inline void ForEachBone(const SkinBonesView& bones, Visitor&& visitor)
    {
        for (std::uint32_t index = 0; index < bones.boneCount; ++index)
        {
            void* const node = bones.boneNodes[index];
            if (node == nullptr)
                continue;

            // The skin update routine points at each node's embedded world
            // transform. Read it through the verified node layout, avoiding
            // an additional array of pointers that may be absent.
            void* worldTransform = reinterpret_cast<std::byte*>(node) +
                Offsets::NiAVObject::kWorldTransform;
            visitor(BoneView{ index, node, worldTransform });
        }
    }

    // Invokes visitor once for every unique, loaded NiSkinInstance reachable
    // below root. A false return means root was null; an otherwise empty walk
    // means the loaded reference currently has no supported skin instance.
    template <class Visitor>
    [[nodiscard]] inline bool ForEachSkinInstance(
        const std::uintptr_t moduleBase,
        void* root,
        Visitor&& visitor)
    {
        if (root == nullptr)
            return false;

        std::vector<void*> pending{ root };
        std::vector<void*> visited;
        std::vector<void*> emittedSkins;
        visited.reserve(128);
        emittedSkins.reserve(16);

        while (!pending.empty() && visited.size() < kMaxSceneObjectsPerWalk)
        {
            void* const object = pending.back();
            pending.pop_back();
            if (object == nullptr || std::find(visited.begin(), visited.end(), object) != visited.end())
                continue;

            visited.push_back(object);

            SkinBonesView bones{};
            bool hasSkin = false;
            if (Detail::DerivesFrom(moduleBase, object, NiTypeDescriptor::kNiGeometryRva) &&
                Detail::IsReadable(object, Offsets::NiGeometry::kSkinInstance + sizeof(void*)))
            {
                void* const skin = Detail::ReadAt<void*>(object, Offsets::NiGeometry::kSkinInstance);
                if (skin != nullptr &&
                    std::find(emittedSkins.begin(), emittedSkins.end(), skin) == emittedSkins.end() &&
                    TryGetSkinBones(moduleBase, skin, bones))
                {
                    hasSkin = true;
                }
            }
            else
            {
                hasSkin = TryGetTriShapeSkinBones(moduleBase, object, bones);
            }

            if (hasSkin &&
                std::find(emittedSkins.begin(), emittedSkins.end(), bones.skinInstance) == emittedSkins.end())
            {
                emittedSkins.push_back(bones.skinInstance);
                visitor(bones);
            }

            if (!Detail::DerivesFrom(moduleBase, object, NiTypeDescriptor::kNiNodeRva))
                continue;
            if (!Detail::IsReadable(object, Offsets::NiNode::kChildrenCount + sizeof(std::uint16_t)))
                continue;

            void** const children = Detail::ReadAt<void**>(object, Offsets::NiNode::kChildrenData);
            const auto childCount = Detail::ReadAt<std::uint16_t>(object, Offsets::NiNode::kChildrenCount);
            if (children == nullptr || childCount == 0 || childCount > kMaxChildrenPerNode ||
                !Detail::IsReadable(children, childCount * sizeof(void*)))
                continue;

            for (std::uint16_t index = 0; index < childCount; ++index)
            {
                if (children[index] != nullptr)
                    pending.push_back(children[index]);
            }
        }

        return true;
    }

    // visitor receives (TESObjectREFR*, SkinBonesView). The strong resolver
    // reference remains owned for the duration of that callback only.  The
    // range arguments permit a renderer to rotate through a large reference
    // list instead of resolving every handle on every frame.
    template <class Visitor>
    [[nodiscard]] inline std::uint32_t ForEachLoadedEntitySkin(
        const std::uintptr_t moduleBase,
        void* tes,
        const std::uint32_t firstHandle,
        const std::uint32_t maxHandles,
        Visitor&& visitor)
    {
        if (tes == nullptr)
            return 0;

        const auto handleCount = Detail::ReadAt<std::uint32_t>(tes, Offsets::TESReferenceList::kHandleCount);
        auto* const handles = Detail::ReadAt<std::uint32_t*>(tes, Offsets::TESReferenceList::kHandleData);
        if (handles == nullptr || handleCount > kMaxReferenceHandles)
            return 0;

        const auto scanCount = std::min(handleCount, maxHandles);
        for (std::uint32_t offset = 0; offset < scanCount; ++offset)
        {
            const auto index = (firstHandle + offset) % handleCount;
            ResolvedReference reference(moduleBase, handles[index]);
            if (!reference)
                continue;

            if (!Detail::HasBaseTypeName(moduleBase, reference.Get(), "Actor@@"))
            {
                continue;
            }

            void* const root = GetLoaded3D(moduleBase, reference.Get());
            static_cast<void>(ForEachSkinInstance(moduleBase, root, [&](const SkinBonesView& bones)
            {
                visitor(reference.Get(), bones);
            }));
        }

        return handleCount;
    }
}
