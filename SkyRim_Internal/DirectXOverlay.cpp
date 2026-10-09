#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <utility>
#include <vector>

#include "DirectXOverlay.hpp"
#include "EntitySkeleton.hpp"
#include "ProjectionMath.hpp"
#include "SkeletonTopology.hpp"
#include "ViewMatrix.hpp"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "gdi32.lib")

namespace
{
    using Microsoft::WRL::ComPtr;

    using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);

    constexpr std::size_t kPresentVtableIndex = 8;
    constexpr std::size_t kMaxTextVertices = 96'000;
    constexpr ULONGLONG kDiscoveryIntervalMs = 100;
    constexpr ULONGLONG kPoseRefreshIntervalMs = 33;
    constexpr std::uint32_t kReferenceChecksPerDiscovery = 32;
    constexpr std::size_t kMaxDeepSceneWalksPerDiscovery = 1;
    constexpr std::size_t kPoseUpdatesPerRefresh = 4;
    constexpr std::size_t kMaxCachedBonesPerEntity = 512;
    constexpr std::size_t kMaxTrackedEntities = 32;
    constexpr std::size_t kMaxFailedCaptures = 128;
    constexpr ULONGLONG kFailedCaptureRetryMs = 2'000;
    constexpr std::size_t kMinBodyBones = 12;
    constexpr float kMaxBoneDistanceFromActor = 800.0f;
    constexpr float kActorTrackingDistance = 6'000.0f;
    constexpr UINT kFontAtlasCellWidth = 16;
    constexpr UINT kFontAtlasCellHeight = 24;
    // Ten digit cells plus one solid white cell for skeleton line quads.
    constexpr UINT kFontAtlasWidth = kFontAtlasCellWidth * 11;
    constexpr UINT kFontAtlasHeight = kFontAtlasCellHeight;
    constexpr float kFontGlyphWidthPixels = 14.0f;
    constexpr float kFontGlyphHeightPixels = 21.0f;
    constexpr float kFontAdvancePixels = 12.0f;

    using Float3 = Skyrim::Projection::Point3;
    using ScreenPoint = Skyrim::Projection::ScreenPoint;
    using CameraSnapshot = Skyrim::Projection::CameraSnapshot;

    struct TextVertex
    {
        float x;
        float y;
        float u;
        float v;
        float r;
        float g;
        float b;
        float a;
    };

    struct CachedBone
    {
        void* node = nullptr;
        Float3 worldPosition;
        std::uint32_t oneBasedIndex;
    };

    struct TrackedEntity
    {
        // The handle is resolved again before reading bones. The root is
        // compared as an identity token; it is never dereferenced after a
        // reference has unloaded.
        std::uint32_t handle = 0;
        void* root = nullptr;
        bool isPlayer = false;
        Skyrim::Actors::VisualState visualState;
        bool visibleLastFrame = true;
        std::vector<CachedBone> bones;
        std::vector<Skyrim::Skeleton::Edge> edges;
        ULONGLONG lastPoseUpdate = 0;
    };

    struct FailedCapture
    {
        std::uint32_t handle = 0;
        ULONGLONG retryAfter = 0;
    };

    using PerfClock = std::chrono::steady_clock;
    [[nodiscard]] double ElapsedMs(const PerfClock::time_point start)
    {
        return std::chrono::duration<double, std::milli>(PerfClock::now() - start).count();
    }

    struct TimingWindow
    {
        std::size_t frames = 0;
        std::size_t queries = 0;
        std::size_t missingCamera = 0;
        double cpuTotal = 0.0;
        double cpuPeak = 0.0;
        double discoveryPeak = 0.0;
        double posePeak = 0.0;
        double projectionTotal = 0.0;
        double drawTotal = 0.0;
    };

    void ConsoleMessage(const char* message)
    {
        DWORD written = 0;
        WriteConsoleA(GetStdHandle(STD_OUTPUT_HANDLE), message,
            static_cast<DWORD>(std::strlen(message)), &written, nullptr);
    }

    template <class T>
    [[nodiscard]] T ReadAt(const void* base, const std::ptrdiff_t offset)
    {
        const auto address = reinterpret_cast<const std::byte*>(base) + offset;
        return *reinterpret_cast<const T*>(address);
    }

    [[nodiscard]] bool IsFinite(const Float3& point)
    {
        return std::isfinite(point.x) && std::isfinite(point.y) && std::isfinite(point.z);
    }

    [[nodiscard]] bool ReadWorldPosition(const void* worldTransform, Float3& position)
    {
        if (worldTransform == nullptr ||
            !Skyrim::Runtime::Detail::IsReadable(worldTransform, Skyrim::Offsets::NiTransform::kSize))
            return false;

        position = {
            ReadAt<float>(worldTransform, Skyrim::Offsets::NiTransform::kTranslation),
            ReadAt<float>(worldTransform, Skyrim::Offsets::NiTransform::kTranslation + sizeof(float)),
            ReadAt<float>(worldTransform, Skyrim::Offsets::NiTransform::kTranslation + 2 * sizeof(float)),
        };
        return IsFinite(position);
    }

    [[nodiscard]] bool ProjectWorld(
        const CameraSnapshot& camera,
        const Float3& world,
        ScreenPoint& projected)
    {
        return Skyrim::Projection::WorldToScreen(camera, world, projected);
    }

    [[nodiscard]] bool IsOnScreen(const CameraSnapshot& camera, const ScreenPoint& point)
    {
        // Include points within a narrow edge margin so feet at the bottom
        // of the viewport can still have their labels moved fully on screen.
        return point.x >= 2.0f * camera.left - 1.04f && point.x <= 2.0f * camera.right - 0.96f &&
            point.y >= 2.0f * camera.bottom - 1.04f && point.y <= 2.0f * camera.top - 0.96f;
    }

    [[nodiscard]] bool ReadActorPosition(const void* reference, Float3& position)
    {
        if (reference == nullptr)
            return false;
        const auto address = reinterpret_cast<const std::byte*>(reference) +
            Skyrim::Offsets::ObjectReference::kPositionX;
        if (!Skyrim::Runtime::Detail::IsReadable(address, sizeof(Float3)))
            return false;
        std::memcpy(&position, address, sizeof(position));
        return IsFinite(position);
    }

    void AppendGlyph(
        std::vector<TextVertex>& vertices,
        const ScreenPoint& anchor,
        const unsigned int digit,
        const float xOffsetPixels,
        const float ndcPerPixelX,
        const float ndcPerPixelY,
        const Skyrim::Actors::Color& color)
    {
        if (vertices.size() + 6 > kMaxTextVertices || digit > 9)
            return;

        // NDC uses an upward-positive Y axis. Place a small Arial label just
        // above and to the right of the bone point.
        const float left = anchor.x + (xOffsetPixels + 3.0f) * ndcPerPixelX;
        const float top = anchor.y + 5.0f * ndcPerPixelY;
        const float right = left + kFontGlyphWidthPixels * ndcPerPixelX;
        const float bottom = top - kFontGlyphHeightPixels * ndcPerPixelY;
        const float u0 = static_cast<float>(digit * kFontAtlasCellWidth) /
            static_cast<float>(kFontAtlasWidth);
        const float u1 = static_cast<float>((digit + 1) * kFontAtlasCellWidth) /
            static_cast<float>(kFontAtlasWidth);

        // White glyphs and black shadows share an atlas. Vertex tint colors
        // labels without rebuilding the texture or issuing extra draw calls.
        vertices.insert(vertices.end(), {
            { left,  top,    u0, 0.0f, color.r, color.g, color.b, color.a },
            { right, top,    u1, 0.0f, color.r, color.g, color.b, color.a },
            { left,  bottom, u0, 1.0f, color.r, color.g, color.b, color.a },
            { right, top,    u1, 0.0f, color.r, color.g, color.b, color.a },
            { right, bottom, u1, 1.0f, color.r, color.g, color.b, color.a },
            { left,  bottom, u0, 1.0f, color.r, color.g, color.b, color.a },
        });
    }

    void AppendBoneIndex(
        std::vector<TextVertex>& vertices,
        const CameraSnapshot& camera,
        const ScreenPoint& anchor,
        std::uint32_t oneBasedIndex,
        const float ndcPerPixelX,
        const float ndcPerPixelY,
        const Skyrim::Actors::Color& color)
    {
        // Render 1, 2, ... 99, 100 with an Arial glyph atlas rather than
        // seven-segment line geometry, which was too thin to read in-game.
        std::uint32_t divisor = 1;
        while (oneBasedIndex / divisor >= 10)
            divisor *= 10;

        const float digitCount = static_cast<float>(
            divisor == 1 ? 1 : (divisor == 10 ? 2 : 3));
        const float textWidth = (digitCount - 1.0f) * kFontAdvancePixels +
            kFontGlyphWidthPixels + 4.0f;
        ScreenPoint labelAnchor = anchor;
        const float minX = 2.0f * camera.left - 1.0f + 2.0f * ndcPerPixelX;
        const float maxX = 2.0f * camera.right - 1.0f - textWidth * ndcPerPixelX;
        const float minY = 2.0f * camera.bottom - 1.0f + (kFontGlyphHeightPixels - 3.0f) * ndcPerPixelY;
        const float maxY = 2.0f * camera.top - 1.0f - 7.0f * ndcPerPixelY;
        if (minX > maxX || minY > maxY)
            return;
        labelAnchor.x = std::clamp(
            labelAnchor.x, minX, maxX);
        labelAnchor.y = std::clamp(
            labelAnchor.y, minY, maxY);

        float xOffsetPixels = 0.0f;
        do
        {
            AppendGlyph(
                vertices,
                labelAnchor,
                (oneBasedIndex / divisor) % 10,
                xOffsetPixels,
                ndcPerPixelX,
                ndcPerPixelY,
                color);
            xOffsetPixels += kFontAdvancePixels;
            divisor /= 10;
        } while (divisor != 0);
    }

    void AppendSkeletonLine(
        std::vector<TextVertex>& vertices, const ScreenPoint& from, const ScreenPoint& to,
        const float ndcPerPixelX, const float ndcPerPixelY,
        const Skyrim::Actors::Color& color)
    {
        if (vertices.size() + 6 > kMaxTextVertices)
            return;
        const float dxPixels = (to.x - from.x) / ndcPerPixelX;
        const float dyPixels = (to.y - from.y) / ndcPerPixelY;
        const float length = std::sqrt(dxPixels * dxPixels + dyPixels * dyPixels);
        if (!std::isfinite(length) || length < 0.25f)
            return;
        constexpr float halfWidthPixels = 1.0f;
        const float nx = -dyPixels / length * halfWidthPixels * ndcPerPixelX;
        const float ny = dxPixels / length * halfWidthPixels * ndcPerPixelY;
        constexpr float u = (10.0f * kFontAtlasCellWidth + kFontAtlasCellWidth * 0.5f) / kFontAtlasWidth;
        constexpr float v = 0.5f;
        const TextVertex a{ from.x + nx, from.y + ny, u, v, color.r, color.g, color.b, color.a };
        const TextVertex b{ from.x - nx, from.y - ny, u, v, color.r, color.g, color.b, color.a };
        const TextVertex c{ to.x + nx, to.y + ny, u, v, color.r, color.g, color.b, color.a };
        const TextVertex d{ to.x - nx, to.y - ny, u, v, color.r, color.g, color.b, color.a };
        vertices.insert(vertices.end(), { a, b, c, c, b, d });
    }

    [[nodiscard]] void* FindNiCamera(const std::uintptr_t moduleBase)
    {
        const auto playerCamera = *reinterpret_cast<void* const*>(
            moduleBase + Skyrim::Offsets::PlayerCamera::kSingletonPointerRva);
        if (playerCamera == nullptr || !Skyrim::Runtime::Detail::IsReadable(
                playerCamera, Skyrim::Offsets::PlayerCamera::kCameraRoot + sizeof(void*)))
            return nullptr;

        const auto cameraRoot = ReadAt<void*>(playerCamera, Skyrim::Offsets::PlayerCamera::kCameraRoot);
        if (cameraRoot == nullptr || !Skyrim::Runtime::Detail::IsReadable(
                cameraRoot, Skyrim::Offsets::NiNode::kChildrenCount + sizeof(std::uint16_t)))
            return nullptr;

        const auto niCameraVtable = moduleBase + Skyrim::Offsets::NiCamera::kVtableRva;
        if (ReadAt<std::uintptr_t>(cameraRoot, 0) == niCameraVtable)
            return cameraRoot;

        // PlayerCamera's active render camera is its direct NiNode child in
        // this Skyrim build. Prefer it: a recursive search can otherwise
        // reach an auxiliary/shadow camera with a valid but wrong matrix.
        void** const directChildren = ReadAt<void**>(cameraRoot, Skyrim::Offsets::NiNode::kChildrenData);
        const auto directChildCount = ReadAt<std::uint16_t>(cameraRoot, Skyrim::Offsets::NiNode::kChildrenCount);
        if (directChildren != nullptr && directChildCount != 0 && directChildCount <= 64 &&
            Skyrim::Runtime::Detail::IsReadable(directChildren, directChildCount * sizeof(void*)))
        {
            for (std::uint16_t index = 0; index < directChildCount; ++index)
            {
                void* const child = directChildren[index];
                if (child != nullptr && Skyrim::Runtime::Detail::IsReadable(child, sizeof(void*)) &&
                    ReadAt<std::uintptr_t>(child, 0) == niCameraVtable)
                    return child;
            }
        }

        return nullptr;
    }

    [[nodiscard]] bool ReadCameraSnapshot(CameraSnapshot& snapshot)
    {
        const auto moduleBase = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
        void* const camera = FindNiCamera(moduleBase);
        if (camera == nullptr || !Skyrim::Runtime::Detail::IsReadable(camera, Skyrim::Offsets::NiCamera::kSize))
            return false;
        std::memcpy(snapshot.matrix, reinterpret_cast<const std::byte*>(camera) +
            Skyrim::Offsets::NiCamera::kViewProjectionMatrix, sizeof(snapshot.matrix));
        snapshot.left = ReadAt<float>(camera, Skyrim::Offsets::NiCamera::kViewportLeft);
        snapshot.right = ReadAt<float>(camera, Skyrim::Offsets::NiCamera::kViewportRight);
        snapshot.top = ReadAt<float>(camera, Skyrim::Offsets::NiCamera::kViewportTop);
        snapshot.bottom = ReadAt<float>(camera, Skyrim::Offsets::NiCamera::kViewportBottom);
        if (!std::isfinite(snapshot.left) || !std::isfinite(snapshot.right) ||
            !std::isfinite(snapshot.top) || !std::isfinite(snapshot.bottom) ||
            snapshot.right <= snapshot.left || snapshot.top <= snapshot.bottom)
            return false;
        for (const float element : snapshot.matrix)
            if (!std::isfinite(element))
                return false;
        return ReadWorldPosition(reinterpret_cast<const std::byte*>(camera) +
            Skyrim::Offsets::NiAVObject::kWorldTransform, snapshot.worldPosition);
    }

    [[nodiscard]] bool CaptureVisibleEntity(
        const std::uintptr_t moduleBase,
        const std::uint32_t handle,
        void* root,
        const Float3& actorPosition,
        const bool isPlayer,
        TrackedEntity& capturedEntity)
    {
        // Discover a skeleton once for this loaded 3D. Put the largest skin
        // first, then add nodes unique to other skins. In particular, feet
        // used only by a separate body/armor mesh must not be dropped.
        std::vector<std::vector<CachedBone>> skinBoneSets;
        static_cast<void>(Skyrim::Runtime::ForEachSkinInstance(
            moduleBase, root, [&](const Skyrim::Runtime::SkinBonesView& skin)
            {
                std::vector<CachedBone> bones;
                bones.reserve(std::min<std::size_t>(skin.boneCount, kMaxCachedBonesPerEntity));
                Skyrim::Runtime::ForEachBone(skin, [&](const Skyrim::Runtime::BoneView& bone)
                {
                    if (bones.size() >= kMaxCachedBonesPerEntity)
                        return;

                    Float3 position{};
                    if (!ReadWorldPosition(bone.worldTransform, position))
                        return;

                    const float dx = position.x - actorPosition.x;
                    const float dy = position.y - actorPosition.y;
                    const float dz = position.z - actorPosition.z;
                    if (dx * dx + dy * dy + dz * dz >
                        kMaxBoneDistanceFromActor * kMaxBoneDistanceFromActor)
                        return;

                    bones.push_back({ bone.node, position, bone.index + 1 });
                });
                if (!bones.empty())
                    skinBoneSets.push_back(std::move(bones));
            }));

        std::sort(skinBoneSets.begin(), skinBoneSets.end(),
            [](const auto& left, const auto& right) { return left.size() > right.size(); });
        if (skinBoneSets.empty() || skinBoneSets.front().size() < kMinBodyBones)
            return false;

        std::vector<CachedBone> mergedBones;
        mergedBones.reserve(kMaxCachedBonesPerEntity);
        for (const auto& skin : skinBoneSets)
        {
            for (const CachedBone& bone : skin)
            {
                if (mergedBones.size() == kMaxCachedBonesPerEntity)
                    break;
                if (std::find_if(mergedBones.begin(), mergedBones.end(),
                        [&](const CachedBone& known) { return known.node == bone.node; }) == mergedBones.end())
                    mergedBones.push_back(bone);
            }
        }

        capturedEntity.handle = handle;
        capturedEntity.root = root;
        capturedEntity.isPlayer = isPlayer;
        std::vector<Skyrim::Skeleton::NodeId> nodes;
        nodes.reserve(mergedBones.size());
        for (const CachedBone& bone : mergedBones)
            nodes.push_back(reinterpret_cast<Skyrim::Skeleton::NodeId>(bone.node));
        capturedEntity.edges = Skyrim::Skeleton::BuildEdges(
            nodes, reinterpret_cast<Skyrim::Skeleton::NodeId>(root),
            [](const Skyrim::Skeleton::NodeId node, Skyrim::Skeleton::NodeId& parent)
            {
                void* parentNode = nullptr;
                if (!Skyrim::Runtime::Detail::TryReadSceneParent(reinterpret_cast<const void*>(node), parentNode))
                    return false;
                parent = reinterpret_cast<Skyrim::Skeleton::NodeId>(parentNode);
                return true;
            });
        capturedEntity.bones = std::move(mergedBones);
        return true;
    }

    [[nodiscard]] bool RefreshTrackedPose(
        const std::uintptr_t moduleBase,
        TrackedEntity& entity,
        const ULONGLONG now)
    {
        Skyrim::Runtime::ResolvedReference reference(moduleBase, entity.handle);
        if (!reference || !Skyrim::Runtime::Detail::TryReadActorVisualState(reference.Get(), entity.visualState) ||
            !entity.visualState.ShouldDraw() ||
            Skyrim::Runtime::GetLoaded3D(moduleBase, reference.Get()) != entity.root)
            return false;

        Float3 center{};
        if (!ReadActorPosition(reference.Get(), center))
            return false;

        for (CachedBone& bone : entity.bones)
        {
            if (bone.node == nullptr)
                return false;
            const auto transform = reinterpret_cast<const std::byte*>(bone.node) +
                Skyrim::Offsets::NiAVObject::kWorldTransform;
            Float3 position{};
            if (!ReadWorldPosition(transform, position))
                return false;

            const float dx = position.x - center.x;
            const float dy = position.y - center.y;
            const float dz = position.z - center.z;
            if (dx * dx + dy * dy + dz * dz >
                kMaxBoneDistanceFromActor * kMaxBoneDistanceFromActor)
                return false;

            bone.worldPosition = position;
        }
        entity.lastPoseUpdate = now;
        return true;
    }

    void BuildOverlayVertices(
        std::vector<TrackedEntity>& trackedEntities,
        const CameraSnapshot& camera,
        const float ndcPerPixelX,
        const float ndcPerPixelY,
        const bool showIndices,
        const bool showSkeleton,
        std::vector<TextVertex>& vertices)
    {
        vertices.clear();

        if (trackedEntities.empty())
            return;

        for (TrackedEntity& entity : trackedEntities)
        {
            entity.visibleLastFrame = false;
            if (!entity.visualState.ShouldDraw())
                continue;
            const auto color = Skyrim::Actors::OverlayColor(entity.visualState, entity.isPlayer);
            if (showSkeleton)
            {
                for (const auto& edge : entity.edges)
                {
                    if (edge.child >= entity.bones.size() || edge.parent >= entity.bones.size())
                        continue;
                    ScreenPoint from{}, to{};
                    if (Skyrim::Projection::ProjectSegment(camera,
                            entity.bones[edge.child].worldPosition, entity.bones[edge.parent].worldPosition, from, to))
                    {
                        entity.visibleLastFrame = true;
                        AppendSkeletonLine(vertices, from, to, ndcPerPixelX, ndcPerPixelY, color);
                    }
                }
            }
            // A visible link already establishes visibility. In skeleton
            // mode avoid projecting the same bones again for hidden labels.
            if (!showIndices && entity.visibleLastFrame)
                continue;
            for (const CachedBone& bone : entity.bones)
            {
                ScreenPoint projected{};
                if (ProjectWorld(camera, bone.worldPosition, projected) &&
                    IsOnScreen(camera, projected))
                {
                    entity.visibleLastFrame = true;
                    if (showIndices && vertices.size() + 18 <= kMaxTextVertices)
                        AppendBoneIndex(
                            vertices, camera, projected, bone.oneBasedIndex,
                            ndcPerPixelX, ndcPerPixelY, color);
                }
            }
        }
    }

    class PipelineState final
    {
    public:
        void Capture(ID3D11DeviceContext* context)
        {
            ID3D11RenderTargetView* renderTarget = nullptr;
            ID3D11DepthStencilView* depthStencil = nullptr;
            context->OMGetRenderTargets(1, &renderTarget, &depthStencil);
            renderTarget_.Attach(renderTarget);
            depthStencil_.Attach(depthStencil);

            context->OMGetBlendState(blendState_.GetAddressOf(), blendFactors_, &sampleMask_);
            context->OMGetDepthStencilState(depthStencilState_.GetAddressOf(), &stencilReference_);
            context->RSGetState(rasterizerState_.GetAddressOf());

            viewportCount_ = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
            context->RSGetViewports(&viewportCount_, viewports_);

            ID3D11Buffer* vertexBuffer = nullptr;
            context->IAGetVertexBuffers(0, 1, &vertexBuffer, &vertexStride_, &vertexOffset_);
            vertexBuffer_.Attach(vertexBuffer);
            context->IAGetInputLayout(inputLayout_.GetAddressOf());
            context->IAGetPrimitiveTopology(&topology_);

            ID3D11VertexShader* vertexShader = nullptr;
            context->VSGetShader(&vertexShader, nullptr, nullptr);
            vertexShader_.Attach(vertexShader);

            ID3D11PixelShader* pixelShader = nullptr;
            context->PSGetShader(&pixelShader, nullptr, nullptr);
            pixelShader_.Attach(pixelShader);
            ID3D11GeometryShader* geometryShader = nullptr;
            context->GSGetShader(&geometryShader, nullptr, nullptr);
            geometryShader_.Attach(geometryShader);
            ID3D11HullShader* hullShader = nullptr;
            context->HSGetShader(&hullShader, nullptr, nullptr);
            hullShader_.Attach(hullShader);
            ID3D11DomainShader* domainShader = nullptr;
            context->DSGetShader(&domainShader, nullptr, nullptr);
            domainShader_.Attach(domainShader);

            ID3D11ShaderResourceView* pixelShaderResource = nullptr;
            context->PSGetShaderResources(0, 1, &pixelShaderResource);
            pixelShaderResource_.Attach(pixelShaderResource);

            ID3D11SamplerState* pixelShaderSampler = nullptr;
            context->PSGetSamplers(0, 1, &pixelShaderSampler);
            pixelShaderSampler_.Attach(pixelShaderSampler);
        }

        void Restore(ID3D11DeviceContext* context) const
        {
            ID3D11RenderTargetView* renderTarget = renderTarget_.Get();
            context->OMSetRenderTargets(1, &renderTarget, depthStencil_.Get());
            context->OMSetBlendState(blendState_.Get(), blendFactors_, sampleMask_);
            context->OMSetDepthStencilState(depthStencilState_.Get(), stencilReference_);
            context->RSSetState(rasterizerState_.Get());
            context->RSSetViewports(viewportCount_, viewportCount_ == 0 ? nullptr : viewports_);

            ID3D11Buffer* vertexBuffer = vertexBuffer_.Get();
            context->IASetVertexBuffers(0, 1, &vertexBuffer, &vertexStride_, &vertexOffset_);
            context->IASetInputLayout(inputLayout_.Get());
            context->IASetPrimitiveTopology(topology_);
            context->VSSetShader(vertexShader_.Get(), nullptr, 0);
            context->PSSetShader(pixelShader_.Get(), nullptr, 0);
            context->GSSetShader(geometryShader_.Get(), nullptr, 0);
            context->HSSetShader(hullShader_.Get(), nullptr, 0);
            context->DSSetShader(domainShader_.Get(), nullptr, 0);
            ID3D11ShaderResourceView* pixelShaderResource = pixelShaderResource_.Get();
            context->PSSetShaderResources(0, 1, &pixelShaderResource);
            ID3D11SamplerState* pixelShaderSampler = pixelShaderSampler_.Get();
            context->PSSetSamplers(0, 1, &pixelShaderSampler);
        }

    private:
        ComPtr<ID3D11RenderTargetView> renderTarget_;
        ComPtr<ID3D11DepthStencilView> depthStencil_;
        ComPtr<ID3D11BlendState> blendState_;
        ComPtr<ID3D11DepthStencilState> depthStencilState_;
        ComPtr<ID3D11RasterizerState> rasterizerState_;
        ComPtr<ID3D11Buffer> vertexBuffer_;
        ComPtr<ID3D11InputLayout> inputLayout_;
        ComPtr<ID3D11VertexShader> vertexShader_;
        ComPtr<ID3D11PixelShader> pixelShader_;
        ComPtr<ID3D11GeometryShader> geometryShader_;
        ComPtr<ID3D11HullShader> hullShader_;
        ComPtr<ID3D11DomainShader> domainShader_;
        ComPtr<ID3D11ShaderResourceView> pixelShaderResource_;
        ComPtr<ID3D11SamplerState> pixelShaderSampler_;
        FLOAT blendFactors_[4]{};
        UINT sampleMask_ = 0;
        UINT stencilReference_ = 0;
        D3D11_VIEWPORT viewports_[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
        UINT viewportCount_ = 0;
        UINT vertexStride_ = 0;
        UINT vertexOffset_ = 0;
        D3D11_PRIMITIVE_TOPOLOGY topology_ = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
    };

    class SkeletonRenderer final
    {
    public:
        void Render(IDXGISwapChain* swapChain)
        {
            Skyrim::Runtime::Detail::ScopedReadabilityCache readability;
            const ULONGLONG now = GetTickCount64();
            if ((GetAsyncKeyState(VK_F6) & 1) != 0)
            {
                showIndices_ = !showIndices_;
                ConsoleMessage(showIndices_ ? "Bone indices ON.\n" : "Bone indices OFF.\n");
            }
            if ((GetAsyncKeyState(VK_F7) & 1) != 0)
            {
                showSkeleton_ = !showSkeleton_;
                ConsoleMessage(showSkeleton_ ? "Skeleton ON.\n" : "Skeleton OFF.\n");
            }
            if ((GetAsyncKeyState(VK_F10) & 1) != 0)
            {
                diagnosticsEnabled_ = !diagnosticsEnabled_;
                timing_ = {};
                lastTimingReport_ = now;
                ConsoleMessage(diagnosticsEnabled_ ? "ESP timing enabled.\n" : "ESP timing disabled.\n");
            }
            const auto frameStart = diagnosticsEnabled_ ? PerfClock::now() : PerfClock::time_point{};
            if (!showIndices_ && !showSkeleton_)
            {
                textVertices_.clear();
                return;
            }
            if (!EnsureDevice(swapChain) || !EnsureRenderTarget(swapChain))
                return;
            const auto measure = [&](auto&& operation)
            {
                if (!diagnosticsEnabled_)
                {
                    operation();
                    return 0.0;
                }
                const auto start = PerfClock::now();
                operation();
                return ElapsedMs(start);
            };

            CameraSnapshot camera{};
            if (!ReadCameraSnapshot(camera))
            {
                textVertices_.clear();
                RecordTiming(now, frameStart, readability.QueryCount(), 0, 0, 0, 0, true);
                return;
            }

            if ((GetAsyncKeyState(VK_F9) & 1) != 0)
            {
                // Rebuild the nearby actor set after a cell or camera change.
                trackedEntities_.clear();
                failedCaptures_.clear();
                textVertices_.clear();
                referenceCursor_ = 0;
                poseCursor_ = 0;
                lastDiscovery_ = 0;
                lastPoseRefresh_ = 0;
            }

            double discoveryMs = 0.0;
            if (now - lastDiscovery_ >= kDiscoveryIntervalMs)
            {
                lastDiscovery_ = now;
                discoveryMs = measure([&] { DiscoverVisibleEntities(now, camera); });
            }

            double poseMs = 0.0;
            if (now - lastPoseRefresh_ >= kPoseRefreshIntervalMs)
            {
                lastPoseRefresh_ = now;
                poseMs = measure([&] { RefreshTrackedEntities(now); });
            }

            if (backBufferDescription_.Width == 0 || backBufferDescription_.Height == 0)
                return;
            const float ndcPerPixelX = 2.0f / static_cast<float>(backBufferDescription_.Width);
            const float ndcPerPixelY = 2.0f / static_cast<float>(backBufferDescription_.Height);
            // Use the current camera every presented frame. The pose cache
            // has its own cadence; camera rotation must not be capped at 30Hz.
            const double projectionMs = measure([&]
            {
                BuildOverlayVertices(
                    trackedEntities_, camera, ndcPerPixelX, ndcPerPixelY,
                    showIndices_, showSkeleton_, textVertices_);
            });

            double drawMs = 0.0;
            if (!textVertices_.empty())
                drawMs = measure([&] { DrawOverlayVertices(); });
            RecordTiming(now, frameStart, readability.QueryCount(), discoveryMs, poseMs, projectionMs, drawMs, false);
        }

        void Shutdown()
        {
            textVertices_.clear();
            textVertexBuffer_.Reset();
            fontSampler_.Reset();
            fontAtlasView_.Reset();
            blendState_.Reset();
            depthStencilState_.Reset();
            rasterizerState_.Reset();
            inputLayout_.Reset();
            vertexShader_.Reset();
            pixelShader_.Reset();
            renderTarget_.Reset();
            backBuffer_.Reset();
            context_.Reset();
            device_.Reset();
            lastDiscovery_ = 0;
            lastPoseRefresh_ = 0;
            referenceCursor_ = 0;
            poseCursor_ = 0;
            trackedEntities_.clear();
            failedCaptures_.clear();
        }

    private:
        void RecordTiming(
            const ULONGLONG now, const PerfClock::time_point start, const std::size_t queries,
            const double discoveryMs, const double poseMs, const double projectionMs, const double drawMs,
            const bool missingCamera)
        {
            if (!diagnosticsEnabled_)
                return;
            const double cpuMs = ElapsedMs(start);
            ++timing_.frames;
            timing_.queries += queries;
            timing_.missingCamera += missingCamera ? 1 : 0;
            timing_.cpuTotal += cpuMs;
            timing_.cpuPeak = std::max(timing_.cpuPeak, cpuMs);
            timing_.discoveryPeak = std::max(timing_.discoveryPeak, discoveryMs);
            timing_.posePeak = std::max(timing_.posePeak, poseMs);
            timing_.projectionTotal += projectionMs;
            timing_.drawTotal += drawMs;
            if (now - lastTimingReport_ < 2'000)
                return;
            const double frames = static_cast<double>(timing_.frames);
            std::size_t boneCount = 0;
            std::size_t linkCount = 0;
            std::size_t teammateCount = 0;
            for (const auto& entity : trackedEntities_)
            {
                boneCount += entity.bones.size();
                linkCount += entity.edges.size();
                if (!entity.isPlayer && entity.visualState.playerTeammate)
                    ++teammateCount;
            }
            char message[512]{};
            std::snprintf(message, sizeof(message),
                "[ESP] actors=%zu teammates=%zu bones=%zu links=%zu vertices=%zu CPU avg/peak=%.3f/%.3f ms scan peak=%.3f pose peak=%.3f project avg=%.3f draw avg=%.3f VQ/frame=%.1f camera missing=%zu\n",
                trackedEntities_.size(), teammateCount, boneCount, linkCount, textVertices_.size(),
                timing_.cpuTotal / frames, timing_.cpuPeak, timing_.discoveryPeak, timing_.posePeak,
                timing_.projectionTotal / frames, timing_.drawTotal / frames,
                static_cast<double>(timing_.queries) / frames, timing_.missingCamera);
            ConsoleMessage(message);
            timing_ = {};
            lastTimingReport_ = now;
        }

        void DiscoverVisibleEntities(const ULONGLONG now, const CameraSnapshot& camera)
        {
            const auto moduleBase = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
            void* const tes = Skyrim::Runtime::GetTESSingleton(moduleBase);
            if (tes == nullptr ||
                !Skyrim::Runtime::Detail::IsReadable(tes, Skyrim::Offsets::TESReferenceList::kHandleCount + sizeof(std::uint32_t)))
                return;

            const auto handleCount = ReadAt<std::uint32_t>(tes, Skyrim::Offsets::TESReferenceList::kHandleCount);
            auto* const handles = ReadAt<std::uint32_t*>(tes, Skyrim::Offsets::TESReferenceList::kHandleData);
            if (handleCount == 0 || handleCount > Skyrim::Runtime::kMaxReferenceHandles || handles == nullptr)
                return;

            std::size_t deepWalks = 0;
            for (std::uint32_t checked = 0; checked < kReferenceChecksPerDiscovery; ++checked)
            {
                const std::uint32_t index = referenceCursor_++ % handleCount;
                if (!Skyrim::Runtime::Detail::IsReadable(handles + index, sizeof(std::uint32_t)))
                    continue;
                const std::uint32_t handle = handles[index];
                if (handle == 0 || std::any_of(trackedEntities_.begin(), trackedEntities_.end(),
                        [&](const TrackedEntity& entity) { return entity.handle == handle; }))
                    continue;
                const auto rejected = std::find_if(
                    failedCaptures_.begin(), failedCaptures_.end(),
                    [&](const FailedCapture& failure) { return failure.handle == handle; });
                if (rejected != failedCaptures_.end() && now < rejected->retryAfter)
                    continue;

                Skyrim::Runtime::ResolvedReference reference(moduleBase, handle);
                if (!reference || !Skyrim::Runtime::Detail::HasBaseTypeName(moduleBase, reference.Get(), "Actor@@"))
                    continue;

                Skyrim::Actors::VisualState visualState{};
                if (!Skyrim::Runtime::Detail::TryReadActorVisualState(reference.Get(), visualState) ||
                    !visualState.ShouldDraw())
                    continue;

                const bool isPlayer = Skyrim::Runtime::Detail::HasBaseTypeName(
                    moduleBase, reference.Get(), "PlayerCharacter@@");
                Float3 position{};
                if (!ReadActorPosition(reference.Get(), position))
                    continue;

                const float dx = position.x - camera.worldPosition.x;
                const float dy = position.y - camera.worldPosition.y;
                const float dz = position.z - camera.worldPosition.z;
                if (!isPlayer && dx * dx + dy * dy + dz * dz > kActorTrackingDistance * kActorTrackingDistance)
                    continue;

                void* const root = Skyrim::Runtime::GetLoaded3D(moduleBase, reference.Get());
                if (root == nullptr)
                    continue;

                ++deepWalks;
                TrackedEntity captured{};
                if (CaptureVisibleEntity(moduleBase, handle, root, position, isPlayer, captured))
                {
                    if (rejected != failedCaptures_.end())
                        failedCaptures_.erase(rejected);
                    captured.visualState = visualState;
                    captured.lastPoseUpdate = now;
                    if (trackedEntities_.size() == kMaxTrackedEntities)
                    {
                        const auto oldest = std::min_element(
                            trackedEntities_.begin(), trackedEntities_.end(),
                            [](const TrackedEntity& left, const TrackedEntity& right)
                            {
                                if (left.isPlayer != right.isPlayer)
                                    return !left.isPlayer;
                                if (left.visibleLastFrame != right.visibleLastFrame)
                                    return !left.visibleLastFrame;
                                return left.lastPoseUpdate < right.lastPoseUpdate;
                            });
                        if (oldest != trackedEntities_.end() && !oldest->isPlayer)
                            trackedEntities_.erase(oldest);
                    }
                    if (trackedEntities_.size() < kMaxTrackedEntities)
                        trackedEntities_.push_back(std::move(captured));
                }
                else if (rejected != failedCaptures_.end())
                {
                    rejected->retryAfter = now + kFailedCaptureRetryMs;
                }
                else
                {
                    if (failedCaptures_.size() == kMaxFailedCaptures)
                        failedCaptures_.erase(failedCaptures_.begin());
                    failedCaptures_.push_back({ handle, now + kFailedCaptureRetryMs });
                }
                if (deepWalks == kMaxDeepSceneWalksPerDiscovery)
                    break;
            }
        }

        void RefreshTrackedEntities(const ULONGLONG now)
        {
            const auto moduleBase = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
            for (std::size_t index = 0; index < trackedEntities_.size();)
            {
                if (!trackedEntities_[index].isPlayer)
                {
                    ++index;
                    continue;
                }
                if (!RefreshTrackedPose(moduleBase, trackedEntities_[index], now))
                    trackedEntities_.erase(trackedEntities_.begin() + index);
                else
                    ++index;
            }

            const std::size_t available = trackedEntities_.size();
            std::size_t visited = 0;
            std::size_t updated = 0;
            while (!trackedEntities_.empty() && visited < available && updated < kPoseUpdatesPerRefresh)
            {
                const std::size_t index = poseCursor_ % trackedEntities_.size();
                poseCursor_ = index + 1;
                ++visited;
                if (trackedEntities_[index].isPlayer)
                    continue;
                // Actors outside the view remain cached, with cheaper pose
                // updates. Turning back does not trigger another scene walk.
                const ULONGLONG interval = trackedEntities_[index].visibleLastFrame ? kPoseRefreshIntervalMs : 500;
                if (now - trackedEntities_[index].lastPoseUpdate < interval)
                    continue;
                ++updated;
                if (!RefreshTrackedPose(moduleBase, trackedEntities_[index], now))
                {
                    trackedEntities_.erase(trackedEntities_.begin() + index);
                    poseCursor_ = index;
                }
            }
        }

        [[nodiscard]] bool EnsureDevice(IDXGISwapChain* swapChain)
        {
            if (device_ != nullptr)
                return true;

            if (FAILED(swapChain->GetDevice(IID_PPV_ARGS(device_.GetAddressOf()))))
                return false;

            device_->GetImmediateContext(context_.GetAddressOf());
            if (context_ == nullptr || !CreatePipeline())
            {
                Shutdown();
                return false;
            }

            return true;
        }

        [[nodiscard]] bool EnsureRenderTarget(IDXGISwapChain* swapChain)
        {
            ComPtr<ID3D11Texture2D> currentBackBuffer;
            if (FAILED(swapChain->GetBuffer(0, IID_PPV_ARGS(currentBackBuffer.GetAddressOf()))))
                return false;

            if (backBuffer_.Get() == currentBackBuffer.Get() && renderTarget_ != nullptr)
                return true;

            renderTarget_.Reset();
            backBuffer_ = currentBackBuffer;
            backBuffer_->GetDesc(&backBufferDescription_);
            return SUCCEEDED(device_->CreateRenderTargetView(backBuffer_.Get(), nullptr, renderTarget_.GetAddressOf()));
        }

        [[nodiscard]] bool CreatePipeline()
        {
            textVertices_.reserve(kMaxTextVertices);
            trackedEntities_.reserve(kMaxTrackedEntities);
            failedCaptures_.reserve(kMaxFailedCaptures);
            constexpr char kShaderSource[] = R"(
                struct VSInput { float2 position : POSITION; float2 uv : TEXCOORD; float4 color : COLOR; };
                struct PSInput { float4 position : SV_POSITION; float2 uv : TEXCOORD; float4 color : COLOR; };
                PSInput VSMain(VSInput input)
                {
                    PSInput output;
                    output.position = float4(input.position, 0.0f, 1.0f);
                    output.uv = input.uv;
                    output.color = input.color;
                    return output;
                }
                Texture2D FontAtlas : register(t0);
                SamplerState FontSampler : register(s0);
                float4 PSMain(PSInput input) : SV_TARGET
                {
                    return FontAtlas.Sample(FontSampler, input.uv) * input.color;
                }
            )";

            ComPtr<ID3DBlob> vertexBytecode;
            ComPtr<ID3DBlob> pixelBytecode;
            if (FAILED(D3DCompile(
                    kShaderSource,
                    std::strlen(kShaderSource),
                    nullptr,
                    nullptr,
                    nullptr,
                    "VSMain",
                    "vs_4_0",
                    D3DCOMPILE_ENABLE_STRICTNESS,
                    0,
                    vertexBytecode.GetAddressOf(),
                    nullptr)) ||
                FAILED(D3DCompile(
                    kShaderSource,
                    std::strlen(kShaderSource),
                    nullptr,
                    nullptr,
                    nullptr,
                    "PSMain",
                    "ps_4_0",
                    D3DCOMPILE_ENABLE_STRICTNESS,
                    0,
                    pixelBytecode.GetAddressOf(),
                    nullptr)))
            {
                return false;
            }

            if (FAILED(device_->CreateVertexShader(
                    vertexBytecode->GetBufferPointer(),
                    vertexBytecode->GetBufferSize(),
                    nullptr,
                    vertexShader_.GetAddressOf())) ||
                FAILED(device_->CreatePixelShader(
                    pixelBytecode->GetBufferPointer(),
                    pixelBytecode->GetBufferSize(),
                    nullptr,
                    pixelShader_.GetAddressOf())))
            {
                return false;
            }

            const D3D11_INPUT_ELEMENT_DESC elements[] = {
                { "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
                { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 8, D3D11_INPUT_PER_VERTEX_DATA, 0 },
                { "COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 16, D3D11_INPUT_PER_VERTEX_DATA, 0 },
            };
            if (FAILED(device_->CreateInputLayout(
                    elements,
                    ARRAYSIZE(elements),
                    vertexBytecode->GetBufferPointer(),
                    vertexBytecode->GetBufferSize(),
                    inputLayout_.GetAddressOf())))
            {
                return false;
            }

            D3D11_BUFFER_DESC vertexBufferDescription{};
            vertexBufferDescription.ByteWidth = static_cast<UINT>(sizeof(TextVertex) * kMaxTextVertices);
            vertexBufferDescription.Usage = D3D11_USAGE_DYNAMIC;
            vertexBufferDescription.BindFlags = D3D11_BIND_VERTEX_BUFFER;
            vertexBufferDescription.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
            if (FAILED(device_->CreateBuffer(&vertexBufferDescription, nullptr, textVertexBuffer_.GetAddressOf())))
                return false;

            D3D11_BLEND_DESC blendDescription{};
            blendDescription.RenderTarget[0].BlendEnable = TRUE;
            blendDescription.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
            blendDescription.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
            blendDescription.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
            blendDescription.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
            blendDescription.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
            blendDescription.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
            blendDescription.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
            if (FAILED(device_->CreateBlendState(&blendDescription, blendState_.GetAddressOf())))
                return false;

            D3D11_DEPTH_STENCIL_DESC depthStencilDescription{};
            depthStencilDescription.DepthEnable = FALSE;
            depthStencilDescription.StencilEnable = FALSE;
            if (FAILED(device_->CreateDepthStencilState(&depthStencilDescription, depthStencilState_.GetAddressOf())))
                return false;

            D3D11_RASTERIZER_DESC rasterizerDescription{};
            rasterizerDescription.FillMode = D3D11_FILL_SOLID;
            rasterizerDescription.CullMode = D3D11_CULL_NONE;
            rasterizerDescription.DepthClipEnable = TRUE;
            rasterizerDescription.ScissorEnable = FALSE;
            if (FAILED(device_->CreateRasterizerState(&rasterizerDescription, rasterizerState_.GetAddressOf())))
                return false;

            return CreateArialFontAtlas();
        }

        [[nodiscard]] bool CreateArialFontAtlas()
        {
            HDC const deviceContext = CreateCompatibleDC(nullptr);
            if (deviceContext == nullptr)
                return false;

            BITMAPINFO bitmapInfo{};
            bitmapInfo.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            bitmapInfo.bmiHeader.biWidth = static_cast<LONG>(kFontAtlasWidth);
            bitmapInfo.bmiHeader.biHeight = -static_cast<LONG>(kFontAtlasHeight);
            bitmapInfo.bmiHeader.biPlanes = 1;
            bitmapInfo.bmiHeader.biBitCount = 32;
            bitmapInfo.bmiHeader.biCompression = BI_RGB;

            void* pixels = nullptr;
            HBITMAP const bitmap = CreateDIBSection(
                deviceContext,
                &bitmapInfo,
                DIB_RGB_COLORS,
                &pixels,
                nullptr,
                0);
            if (bitmap == nullptr || pixels == nullptr)
            {
                DeleteDC(deviceContext);
                return false;
            }

            HGDIOBJ const previousBitmap = SelectObject(deviceContext, bitmap);
            HFONT const arial = CreateFontW(
                -18,
                0,
                0,
                0,
                FW_BOLD,
                FALSE,
                FALSE,
                FALSE,
                DEFAULT_CHARSET,
                OUT_DEFAULT_PRECIS,
                CLIP_DEFAULT_PRECIS,
                ANTIALIASED_QUALITY,
                DEFAULT_PITCH | FF_DONTCARE,
                L"Arial");
            if (arial == nullptr)
            {
                SelectObject(deviceContext, previousBitmap);
                DeleteObject(bitmap);
                DeleteDC(deviceContext);
                return false;
            }

            HGDIOBJ const previousFont = SelectObject(deviceContext, arial);
            std::memset(pixels, 0, kFontAtlasWidth * kFontAtlasHeight * sizeof(std::uint32_t));
            SetBkMode(deviceContext, TRANSPARENT);
            SetTextColor(deviceContext, RGB(255, 255, 255));

            for (unsigned int digit = 0; digit < 10; ++digit)
            {
                const wchar_t glyph[] = { static_cast<wchar_t>(L'0' + digit), L'\0' };
                RECT cell{
                    static_cast<LONG>(digit * kFontAtlasCellWidth),
                    0,
                    static_cast<LONG>((digit + 1) * kFontAtlasCellWidth),
                    static_cast<LONG>(kFontAtlasCellHeight),
                };
                DrawTextW(
                    deviceContext,
                    glyph,
                    1,
                    &cell,
                    DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
            }

            // GDI can batch drawing. Finish it before reading the DIB pixels.
            GdiFlush();
            std::vector<std::uint32_t> atlasPixels(kFontAtlasWidth * kFontAtlasHeight);
            const auto* coverage = static_cast<const std::uint32_t*>(pixels);
            for (UINT y = 0; y < kFontAtlasHeight; ++y)
            {
                for (UINT x = 0; x < kFontAtlasWidth; ++x)
                {
                    if (x >= 10 * kFontAtlasCellWidth)
                    {
                        atlasPixels[y * kFontAtlasWidth + x] = 0xFFFFFFFF;
                        continue;
                    }
                    const UINT foreground = coverage[y * kFontAtlasWidth + x] & 0xFF;
                    const UINT shadow = y > 0 && x % kFontAtlasCellWidth > 0
                        ? coverage[(y - 1) * kFontAtlasWidth + x - 1] & 0xFF : 0;
                    const UINT alpha = foreground + shadow * (255 - foreground) / 255;
                    const UINT intensity = alpha != 0 ? foreground * 255 / alpha : 0;
                    atlasPixels[y * kFontAtlasWidth + x] =
                        (alpha << 24) | (intensity << 16) | (intensity << 8) | intensity;
                }
            }

            D3D11_TEXTURE2D_DESC textureDescription{};
            textureDescription.Width = kFontAtlasWidth;
            textureDescription.Height = kFontAtlasHeight;
            textureDescription.MipLevels = 1;
            textureDescription.ArraySize = 1;
            textureDescription.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            textureDescription.SampleDesc.Count = 1;
            textureDescription.Usage = D3D11_USAGE_DEFAULT;
            textureDescription.BindFlags = D3D11_BIND_SHADER_RESOURCE;

            D3D11_SUBRESOURCE_DATA textureData{};
            textureData.pSysMem = atlasPixels.data();
            textureData.SysMemPitch = kFontAtlasWidth * sizeof(std::uint32_t);
            ComPtr<ID3D11Texture2D> fontTexture;
            const HRESULT textureResult = device_->CreateTexture2D(
                &textureDescription,
                &textureData,
                fontTexture.GetAddressOf());
            const HRESULT viewResult = SUCCEEDED(textureResult)
                ? device_->CreateShaderResourceView(fontTexture.Get(), nullptr, fontAtlasView_.GetAddressOf())
                : textureResult;

            D3D11_SAMPLER_DESC samplerDescription{};
            samplerDescription.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
            samplerDescription.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
            samplerDescription.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
            samplerDescription.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
            samplerDescription.MaxLOD = D3D11_FLOAT32_MAX;
            const HRESULT samplerResult = SUCCEEDED(viewResult)
                ? device_->CreateSamplerState(&samplerDescription, fontSampler_.GetAddressOf())
                : viewResult;

            SelectObject(deviceContext, previousFont);
            DeleteObject(arial);
            SelectObject(deviceContext, previousBitmap);
            DeleteObject(bitmap);
            DeleteDC(deviceContext);
            return SUCCEEDED(samplerResult);
        }

        void DrawOverlayVertices()
        {
            const auto vertexCount = static_cast<UINT>(std::min(textVertices_.size(), kMaxTextVertices));
            if (vertexCount < 3 || fontAtlasView_ == nullptr || fontSampler_ == nullptr)
                return;

            PipelineState previous;
            previous.Capture(context_.Get());

            D3D11_MAPPED_SUBRESOURCE mapped{};
            if (SUCCEEDED(context_->Map(textVertexBuffer_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
            {
                std::memcpy(mapped.pData, textVertices_.data(), vertexCount * sizeof(TextVertex));
                context_->Unmap(textVertexBuffer_.Get(), 0);

                D3D11_VIEWPORT viewport{};
                viewport.Width = static_cast<float>(backBufferDescription_.Width);
                viewport.Height = static_cast<float>(backBufferDescription_.Height);
                viewport.MinDepth = 0.0f;
                viewport.MaxDepth = 1.0f;

                ID3D11RenderTargetView* renderTarget = renderTarget_.Get();
                const UINT stride = sizeof(TextVertex);
                constexpr UINT offset = 0;
                ID3D11Buffer* vertexBuffer = textVertexBuffer_.Get();
                constexpr float blendFactors[4] = { 0.0f, 0.0f, 0.0f, 0.0f };

                context_->OMSetRenderTargets(1, &renderTarget, nullptr);
                context_->OMSetBlendState(blendState_.Get(), blendFactors, 0xFFFFFFFF);
                context_->OMSetDepthStencilState(depthStencilState_.Get(), 0);
                context_->RSSetState(rasterizerState_.Get());
                context_->RSSetViewports(1, &viewport);
                context_->IASetInputLayout(inputLayout_.Get());
                context_->IASetVertexBuffers(0, 1, &vertexBuffer, &stride, &offset);
                context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                context_->VSSetShader(vertexShader_.Get(), nullptr, 0);
                context_->PSSetShader(pixelShader_.Get(), nullptr, 0);
                context_->GSSetShader(nullptr, nullptr, 0);
                context_->HSSetShader(nullptr, nullptr, 0);
                context_->DSSetShader(nullptr, nullptr, 0);
                ID3D11ShaderResourceView* fontAtlas = fontAtlasView_.Get();
                context_->PSSetShaderResources(0, 1, &fontAtlas);
                ID3D11SamplerState* fontSampler = fontSampler_.Get();
                context_->PSSetSamplers(0, 1, &fontSampler);
                context_->Draw(vertexCount, 0);
            }

            previous.Restore(context_.Get());
        }

        ComPtr<ID3D11Device> device_;
        ComPtr<ID3D11DeviceContext> context_;
        ComPtr<ID3D11Texture2D> backBuffer_;
        D3D11_TEXTURE2D_DESC backBufferDescription_{};
        ComPtr<ID3D11RenderTargetView> renderTarget_;
        ComPtr<ID3D11VertexShader> vertexShader_;
        ComPtr<ID3D11PixelShader> pixelShader_;
        ComPtr<ID3D11InputLayout> inputLayout_;
        ComPtr<ID3D11Buffer> textVertexBuffer_;
        ComPtr<ID3D11ShaderResourceView> fontAtlasView_;
        ComPtr<ID3D11SamplerState> fontSampler_;
        ComPtr<ID3D11BlendState> blendState_;
        ComPtr<ID3D11DepthStencilState> depthStencilState_;
        ComPtr<ID3D11RasterizerState> rasterizerState_;
        std::vector<TextVertex> textVertices_;
        std::vector<TrackedEntity> trackedEntities_;
        std::vector<FailedCapture> failedCaptures_;
        ULONGLONG lastDiscovery_ = 0;
        ULONGLONG lastPoseRefresh_ = 0;
        std::uint32_t referenceCursor_ = 0;
        std::size_t poseCursor_ = 0;
        bool showIndices_ = false;
        bool showSkeleton_ = true;
        bool diagnosticsEnabled_ = false;
        ULONGLONG lastTimingReport_ = 0;
        TimingWindow timing_{};
    };

    std::atomic<PresentFn> g_originalPresent = nullptr;
    std::atomic_bool g_installed = false;
    std::atomic_bool g_unloading = false;
    std::atomic_bool g_enabled = false;
    std::atomic_bool g_presentObserved = false;
    std::atomic_uint32_t g_activePresentCalls = 0;
    void** g_presentSlot = nullptr;
    std::mutex g_rendererMutex;
    SkeletonRenderer g_renderer;

    HRESULT STDMETHODCALLTYPE PresentHook(IDXGISwapChain* swapChain, const UINT syncInterval, const UINT flags)
    {
        const auto original = g_originalPresent.load(std::memory_order_acquire);
        if (original == nullptr)
            return DXGI_ERROR_INVALID_CALL;

        g_activePresentCalls.fetch_add(1, std::memory_order_acq_rel);
        if (!g_unloading.load(std::memory_order_acquire) && (flags & DXGI_PRESENT_TEST) == 0)
        {
            if (!g_presentObserved.exchange(true, std::memory_order_acq_rel))
            {
                constexpr char kPresentMessage[] = "DX11: game Present is executing.\n";
                DWORD written = 0;
                WriteConsoleA(
                    GetStdHandle(STD_OUTPUT_HANDLE),
                    kPresentMessage,
                    static_cast<DWORD>(sizeof(kPresentMessage) - 1),
                    &written,
                    nullptr);
            }

            try
            {
                if ((GetAsyncKeyState(VK_INSERT) & 1) != 0)
                    g_enabled.store(!g_enabled.load(std::memory_order_relaxed), std::memory_order_release);

                if (g_enabled.load(std::memory_order_acquire))
                {
                    std::scoped_lock lock(g_rendererMutex);
                    g_renderer.Render(swapChain);
                }
            }
            catch (...)
            {
                // Present must always fall through to the game's original call.
            }
        }

        g_activePresentCalls.fetch_sub(1, std::memory_order_acq_rel);
        return original(swapChain, syncInterval, flags);
    }

    [[nodiscard]] bool PatchPresentSlot(void** slot, void* target)
    {
        DWORD oldProtection = 0;
        if (!VirtualProtect(slot, sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProtection))
            return false;

        InterlockedExchangePointer(reinterpret_cast<PVOID volatile*>(slot), target);

        DWORD ignoredProtection = 0;
        VirtualProtect(slot, sizeof(void*), oldProtection, &ignoredProtection);
        return true;
    }
}

bool Skyrim::Overlay::Install()
{
    std::scoped_lock lock(g_rendererMutex);
    if (g_installed.load(std::memory_order_acquire))
        return true;

    HWND const dummyWindow = CreateWindowExA(
        0,
        "STATIC",
        "SkyrimOverlayDummy",
        WS_OVERLAPPED,
        0,
        0,
        1,
        1,
        nullptr,
        nullptr,
        GetModuleHandleW(nullptr),
        nullptr);
    if (dummyWindow == nullptr)
        return false;

    DXGI_SWAP_CHAIN_DESC description{};
    description.BufferDesc.Width = 1;
    description.BufferDesc.Height = 1;
    description.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    description.SampleDesc.Count = 1;
    description.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    description.BufferCount = 1;
    description.OutputWindow = dummyWindow;
    description.Windowed = TRUE;
    description.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<IDXGISwapChain> swapChain;
    const HRESULT createResult = D3D11CreateDeviceAndSwapChain(
        nullptr,
        D3D_DRIVER_TYPE_HARDWARE,
        nullptr,
        0,
        nullptr,
        0,
        D3D11_SDK_VERSION,
        &description,
        swapChain.GetAddressOf(),
        device.GetAddressOf(),
        nullptr,
        context.GetAddressOf());
    DestroyWindow(dummyWindow);
    if (FAILED(createResult) || swapChain == nullptr)
        return false;

    auto** const vtable = *reinterpret_cast<void***>(swapChain.Get());
    if (vtable == nullptr || vtable[kPresentVtableIndex] == nullptr)
        return false;

    const auto original = std::bit_cast<PresentFn>(vtable[kPresentVtableIndex]);
    const auto hook = std::bit_cast<void*>(static_cast<PresentFn>(&PresentHook));

    // Publish the trampoline before exposing the hook.  Present can run on
    // another thread as soon as the vtable entry is replaced.
    g_originalPresent.store(original, std::memory_order_release);
    g_unloading.store(false, std::memory_order_release);
    if (!PatchPresentSlot(&vtable[kPresentVtableIndex], hook))
    {
        g_originalPresent.store(nullptr, std::memory_order_release);
        return false;
    }

    g_presentSlot = &vtable[kPresentVtableIndex];
    g_presentObserved.store(false, std::memory_order_release);
    // Enable the first run automatically.  The yellow renderer probe makes
    // it immediately obvious whether the DX11 path is active; Insert still
    // toggles the overlay afterwards.
    g_enabled.store(true, std::memory_order_release);
    g_installed.store(true, std::memory_order_release);
    return true;
}

void Skyrim::Overlay::Shutdown()
{
    if (!g_installed.exchange(false, std::memory_order_acq_rel))
        return;

    g_enabled.store(false, std::memory_order_release);
    g_unloading.store(true, std::memory_order_release);

    const auto original = g_originalPresent.load(std::memory_order_acquire);
    if (g_presentSlot != nullptr && original != nullptr)
        static_cast<void>(PatchPresentSlot(g_presentSlot, std::bit_cast<void*>(original)));

    for (int attempt = 0; attempt < 200 && g_activePresentCalls.load(std::memory_order_acquire) != 0; ++attempt)
        Sleep(10);

    std::scoped_lock lock(g_rendererMutex);
    g_renderer.Shutdown();
    g_presentSlot = nullptr;
    g_originalPresent.store(nullptr, std::memory_order_release);
}

bool Skyrim::Overlay::IsEnabled()
{
    return g_enabled.load(std::memory_order_acquire);
}
