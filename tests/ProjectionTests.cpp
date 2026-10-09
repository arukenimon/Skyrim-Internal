#include "../SkyRim_Internal/ProjectionMath.hpp"
#include "../SkyRim_Internal/EntitySkeleton.hpp"
#include "../SkyRim_Internal/SkeletonTopology.hpp"

#include <cstdio>
#include <cstdlib>
#include <limits>

using namespace Skyrim::Projection;

static void Require(bool condition, const char* name)
{
    if (!condition)
    {
        std::printf("FAIL: %s\n", name);
        std::exit(1);
    }
}

static bool Near(float actual, float expected)
{
    return std::abs(actual - expected) < 0.0005f;
}

static float Dot(const Point3& a, const Point3& b)
{
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

static CameraSnapshot Camera(float yaw, float pitch)
{
    const Point3 forward{ std::cos(pitch) * std::cos(yaw), std::cos(pitch) * std::sin(yaw), std::sin(pitch) };
    const Point3 right{ -std::sin(yaw), std::cos(yaw), 0.0f };
    const Point3 up{ -std::sin(pitch) * std::cos(yaw), -std::sin(pitch) * std::sin(yaw), std::cos(pitch) };
    CameraSnapshot result;
    result.worldPosition = { 1234.0f, -678.0f, 321.0f };
    const Point3 rows[]{ right, up, Point3{}, forward };
    for (int row = 0; row < 4; ++row)
    {
        result.matrix[row * 4] = rows[row].x;
        result.matrix[row * 4 + 1] = rows[row].y;
        result.matrix[row * 4 + 2] = rows[row].z;
        result.matrix[row * 4 + 3] = -Dot(rows[row], result.worldPosition);
    }
    return result;
}

static Point3 WorldPoint(const CameraSnapshot& camera, float depth, float right, float up)
{
    const float* m = camera.matrix;
    return {
        camera.worldPosition.x + depth * m[12] + right * m[0] + up * m[4],
        camera.worldPosition.y + depth * m[13] + right * m[1] + up * m[5],
        camera.worldPosition.z + depth * m[14] + right * m[2] + up * m[6],
    };
}

int main()
{
    int rotations = 0;
    for (int yawDegrees = 0; yawDegrees < 360; yawDegrees += 5)
    {
        for (int pitchDegrees = -85; pitchDegrees <= 85; pitchDegrees += 5)
        {
            const auto camera = Camera(yawDegrees * 0.01745329252f, pitchDegrees * 0.01745329252f);
            ScreenPoint output{};
            Require(WorldToScreen(camera, WorldPoint(camera, 100.0f, 20.0f, 30.0f), output), "visible at every yaw/pitch");
            Require(Near(output.x, 0.2f) && Near(output.y, 0.3f), "rotation-invariant screen coordinates");
            Require(!WorldToScreen(camera, WorldPoint(camera, -100.0f, 20.0f, 30.0f), output), "reject behind camera");
            ScreenPoint segmentFrom{}, segmentTo{};
            Require(ProjectSegment(camera, WorldPoint(camera, 100.0f, 20.0f, 30.0f),
                WorldPoint(camera, 100.0f, -20.0f, -30.0f), segmentFrom, segmentTo), "limb projects at every yaw/pitch");
            Require(Near(segmentFrom.x, 0.2f) && Near(segmentFrom.y, 0.3f) &&
                Near(segmentTo.x, -0.2f) && Near(segmentTo.y, -0.3f), "limb projection matches bone points");
            ++rotations;
        }
    }
    auto camera = Camera(0.8f, -0.4f);
    ScreenPoint output{};
    camera.left = 0.25f; camera.right = 0.75f;
    camera.bottom = 0.1f; camera.top = 0.9f;
    Require(WorldToScreen(camera, WorldPoint(camera, 100.0f, 20.0f, 30.0f), output), "partial viewport projection");
    Require(Near(output.x, 0.1f) && Near(output.y, 0.24f), "camera viewport mapped to backbuffer");

    camera = Camera(0.0f, 0.0f);
    Require(WorldToScreen(camera, WorldPoint(camera, 100.0f, 0.0f, -100.0f), output) && Near(output.y, -1.0f), "foot at bottom edge");
    Require(WorldToScreen(camera, WorldPoint(camera, 100.0f, 200.0f, 0.0f), output) && Near(output.x, 2.0f), "projection independent of screen culling");
    Require(!WorldToScreen(camera, camera.worldPosition, output), "zero clip W");
    Require(!WorldToScreen(camera, { std::numeric_limits<float>::quiet_NaN(), 0.0f, 0.0f }, output), "reject non-finite input");

    const Point3 target = WorldPoint(camera, 100.0f, 20.0f, 30.0f);
    // An asymmetric frustum adds an offset proportional to forward depth.
    for (int column = 0; column < 4; ++column)
    {
        camera.matrix[column] += 0.15f * camera.matrix[12 + column];
        camera.matrix[4 + column] -= 0.1f * camera.matrix[12 + column];
    }
    Require(WorldToScreen(camera, target, output) && Near(output.x, 0.35f) && Near(output.y, 0.2f), "asymmetric frustum");

    camera = Camera(0.0f, 0.0f);
    ScreenPoint lineFrom{}, lineTo{};
    Require(ProjectSegment(camera, WorldPoint(camera, 100.0f, -200.0f, 0.0f),
        WorldPoint(camera, 100.0f, 200.0f, 0.0f), lineFrom, lineTo), "limb crosses both screen edges");
    Require(Near(lineFrom.x, -1.0f) && Near(lineTo.x, 1.0f), "clip limb to viewport horizontally");
    Require(ProjectSegment(camera, WorldPoint(camera, 100.0f, 0.0f, -200.0f),
        WorldPoint(camera, 100.0f, 0.0f, 0.0f), lineFrom, lineTo) && Near(lineFrom.y, -1.0f), "clip foot connection at bottom edge");
    Require(!ProjectSegment(camera, WorldPoint(camera, 100.0f, 200.0f, 0.0f),
        WorldPoint(camera, 100.0f, 300.0f, 0.0f), lineFrom, lineTo), "reject fully off-screen limb");
    Require(!ProjectSegment(camera, WorldPoint(camera, -100.0f, 0.0f, 0.0f),
        WorldPoint(camera, -200.0f, 0.0f, 0.0f), lineFrom, lineTo), "reject limb entirely behind camera");
    Require(ProjectSegment(camera, WorldPoint(camera, -100.0f, 0.0f, 0.0f),
        WorldPoint(camera, 100.0f, 0.0f, 0.0f), lineFrom, lineTo) &&
        std::isfinite(lineFrom.x) && std::isfinite(lineTo.x), "clip crossing near plane without inverted line");
    camera.left = 0.25f; camera.right = 0.75f;
    Require(ProjectSegment(camera, WorldPoint(camera, 100.0f, -200.0f, 0.0f),
        WorldPoint(camera, 100.0f, 200.0f, 0.0f), lineFrom, lineTo) &&
        Near(lineFrom.x, -0.5f) && Near(lineTo.x, 0.5f), "clip to partial camera viewport");

    using Skyrim::Skeleton::NodeId;
    const std::vector<NodeId> boneNodes{ 20, 40, 50 };
    const std::unordered_map<NodeId, NodeId> hierarchy{
        {20, 10}, {40, 30}, {30, 20}, {50, 40}, {70, 80}, {80, 70},
    };
    std::size_t parentReads = 0;
    const auto parentReader = [&](const NodeId node, NodeId& parent)
    {
        Require(node != 10, "never traverse outside actor root");
        ++parentReads;
        const auto found = hierarchy.find(node);
        if (found == hierarchy.end()) return false;
        parent = found->second;
        return true;
    };
    const auto edges = Skyrim::Skeleton::BuildEdges(boneNodes, 10, parentReader);
    Require(edges.size() == 2 && edges[0].child == 1 && edges[0].parent == 0 &&
        edges[1].child == 2 && edges[1].parent == 1, "connect nearest included ancestor and foot chain");
    Require(parentReads == 4, "cache intermediate parent reads during topology capture");
    const std::vector<NodeId> cycle{ 70, 80 };
    Require(Skyrim::Skeleton::BuildEdges(cycle, 10, parentReader).empty(), "reject cyclic skeleton hierarchy");
    const std::vector<NodeId> unreadable{ 40, 50 };
    Require(Skyrim::Skeleton::BuildEdges(unreadable, 10,
        [&](const NodeId node, NodeId& parent) { return node != 30 && parentReader(node, parent); }).empty(),
        "reject unreadable chain before connecting unrelated nodes");
    const std::vector<NodeId> duplicate{ 20, 40, 40, 50 };
    Require(Skyrim::Skeleton::BuildEdges(duplicate, 10, parentReader).size() == 2, "avoid duplicate bone edges");

    // Independent fixture of the layout seen in the game setter. This test
    // uses the same memory reader as capture, so a regression to +0x40
    // cannot pass merely because the abstract parent-graph tests pass.
    struct SceneNodeFixture
    {
        std::array<std::byte, 0x30> objectNet{};
        SceneNodeFixture* parent = nullptr;
        std::uint64_t member38 = 0;
        void* collision = nullptr;
    };
    static_assert(offsetof(SceneNodeFixture, parent) == 0x30);
    static_assert(offsetof(SceneNodeFixture, collision) == 0x40);
    SceneNodeFixture actorRoot{}, pelvis{}, spine{}, head{};
    pelvis.parent = &actorRoot;
    spine.parent = &pelvis;
    head.parent = &spine;
    const std::vector<NodeId> sceneBones{
        reinterpret_cast<NodeId>(&pelvis), reinterpret_cast<NodeId>(&spine), reinterpret_cast<NodeId>(&head),
    };
    const auto sceneEdges = Skyrim::Skeleton::BuildEdges(sceneBones, reinterpret_cast<NodeId>(&actorRoot),
        [](const NodeId node, NodeId& parent)
        {
            void* parentNode = nullptr;
            if (!Skyrim::Runtime::Detail::TryReadSceneParent(reinterpret_cast<const void*>(node), parentNode))
                return false;
            parent = reinterpret_cast<NodeId>(parentNode);
            return true;
        });
    Require(sceneEdges.size() == 2 && sceneEdges[0].child == 1 && sceneEdges[0].parent == 0 &&
        sceneEdges[1].child == 2 && sceneEdges[1].parent == 1, "read real-layout parent at +0x30, not collision at +0x40");

    // Independent fixture of the two fields read by the verified Papyrus
    // accessors. Exercise the runtime reader, not only the pure decoder.
    struct ActorFixture
    {
        std::array<std::byte, 0xC8> prefix{};
        std::uint32_t lifeStateFlags = 0;
        std::array<std::byte, 0x1C> gap{};
        std::uint32_t actorFlags = 0;
    } actor;
    static_assert(offsetof(ActorFixture, lifeStateFlags) == 0xC8);
    static_assert(offsetof(ActorFixture, actorFlags) == 0xE8);
    Skyrim::Actors::VisualState actorState{};
    {
        Skyrim::Runtime::Detail::ScopedReadabilityCache cache;
        for (std::uint32_t life = 0; life < 16; ++life)
        {
            for (std::uint32_t team = 0; team < 2; ++team)
            {
                actor.lifeStateFlags = (life << 21) | 0xE01F'FFFF;
                // All unrelated bits are set to catch overly broad masks.
                actor.actorFlags = 0xFBFF'FFFF | (team << 26);
                Require(Skyrim::Runtime::Detail::TryReadActorVisualState(&actor, actorState), "actor state fields readable");
                const bool expectedDead = life == 1 || life == 2 || life == 5;
                Require(actorState.dead == expectedDead && actorState.ShouldDraw() == !expectedDead,
                    "match engine IsDead(true), not arbitrary health or state bits");
                Require(actorState.playerTeammate == (team != 0), "only flag bit 26 means player teammate");
            }
        }
        Require(cache.QueryCount() == 1, "actor classification reuses region check, no engine relationship scans");
    }
    actor.lifeStateFlags = 2U << 21;
    actor.actorFlags = 1U << 26;
    Require(Skyrim::Runtime::Detail::TryReadActorVisualState(&actor, actorState) &&
        !actorState.ShouldDraw(), "dead teammates also hidden");
    actor.lifeStateFlags = 0;
    Require(Skyrim::Runtime::Detail::TryReadActorVisualState(&actor, actorState) &&
        actorState.ShouldDraw() && actorState.playerTeammate, "resurrection is not permanently excluded");
    const auto teammateColor = Skyrim::Actors::OverlayColor(actorState, false);
    Require(teammateColor.g > teammateColor.r && teammateColor.g > teammateColor.b, "teammate is green");
    const auto playerColor = Skyrim::Actors::OverlayColor(actorState, true);
    Require(playerColor.b > playerColor.g && playerColor.b > playerColor.r, "self is blue even with teammate flag");
    actor.actorFlags = 0;
    Require(Skyrim::Runtime::Detail::TryReadActorVisualState(&actor, actorState) &&
        !actorState.playerTeammate, "teammate dismissal refreshes classification");
    const auto otherColor = Skyrim::Actors::OverlayColor(actorState, false);
    Require(otherColor.g > otherColor.r && otherColor.b > otherColor.r, "other NPC is cyan, not assumed enemy");
    Require(!Skyrim::Runtime::Detail::TryReadActorVisualState(nullptr, actorState), "null actor rejected");

    // Exercise the callback-local memory check cache against real page
    // protections, including a protection change between callbacks.
    SYSTEM_INFO systemInfo{};
    GetSystemInfo(&systemInfo);
    auto* pages = static_cast<std::byte*>(VirtualAlloc(nullptr, systemInfo.dwPageSize * 2,
        MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    Require(pages != nullptr, "allocate readability test pages");
    DWORD oldProtection = 0;
    Require(VirtualProtect(pages + systemInfo.dwPageSize, systemInfo.dwPageSize,
        PAGE_NOACCESS, &oldProtection) != 0, "guard second test page");
    {
        Skyrim::Runtime::Detail::ScopedReadabilityCache cache;
        for (int index = 0; index < 1000; ++index)
            Require(Skyrim::Runtime::Detail::IsReadable(pages + index, 1), "committed page readable");
        Require(!Skyrim::Runtime::Detail::IsReadable(pages + systemInfo.dwPageSize, 1), "noaccess page rejected");
        Require(!Skyrim::Runtime::Detail::TryReadActorVisualState(pages + systemInfo.dwPageSize, actorState),
            "unreadable actor state rejected safely");
        Require(cache.QueryCount() == 2, "region checks avoid repeated VirtualQuery calls");
    }
    Require(VirtualProtect(pages, systemInfo.dwPageSize, PAGE_NOACCESS, &oldProtection) != 0,
        "change protection between callbacks");
    {
        Skyrim::Runtime::Detail::ScopedReadabilityCache cache;
        Require(!Skyrim::Runtime::Detail::IsReadable(pages, 1), "new callback sees changed protection");
        Require(cache.QueryCount() == 1, "new callback refreshes region data");
    }
    Require(VirtualFree(pages, 0, MEM_RELEASE) != 0, "release test pages");
    std::printf("PASS: %d yaw/pitch combinations plus viewport, foot edge, asymmetric frustum and clip cases.\n", rotations);
    std::printf("PASS: memory protection changes and callback-scoped region caching.\n");
    std::printf("PASS: skeleton topology, parent field ABI, omitted ancestors, cycles and limb clipping.\n");
    std::printf("PASS: actor state ABI, dead filtering, teammate flag, resurrection, dismissal and colors.\n");
}
