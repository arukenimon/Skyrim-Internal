#pragma once

#include <algorithm>
#include <cmath>

namespace Skyrim::Projection
{
    struct Point3 { float x, y, z; };
    struct ScreenPoint { float x, y; }; // D3D normalized device coordinates

    struct CameraSnapshot
    {
        float matrix[16]{};
        // NiCamera's normalized viewport, with upward-positive Y.
        float left = 0.0f;
        float right = 1.0f;
        float top = 1.0f;
        float bottom = 0.0f;
        Point3 worldPosition{};
    };

    // Matches Skyrim's NiCamera projection routine at RVA 0xD2DE10:
    // X = dot(row 0, world), Y = dot(row 1, world), W = dot(row 3, world).
    // The routine then maps X/W and Y/W through the camera's viewport.
    // Keep screen clipping separate: an off-screen point is still projectable.
    [[nodiscard]] inline bool WorldToScreen(
        const CameraSnapshot& camera, const Point3& world, ScreenPoint& output)
    {
        const float* m = camera.matrix;
        const float w = world.x * m[12] + world.y * m[13] + world.z * m[14] + m[15];
        if (!std::isfinite(w) || w <= 0.001f)
            return false;
        const float x = (world.x * m[0] + world.y * m[1] + world.z * m[2] + m[3]) / w;
        const float y = (world.x * m[4] + world.y * m[5] + world.z * m[6] + m[7]) / w;
        output.x = (camera.right - camera.left) * x + camera.right + camera.left - 1.0f;
        output.y = (camera.top - camera.bottom) * y + camera.top + camera.bottom - 1.0f;
        return std::isfinite(output.x) && std::isfinite(output.y);
    }

    // Clip the segment in homogeneous space before division by W. This
    // preserves a limb crossing the screen edge, and prevents long inverted
    // lines when one endpoint is behind the camera.
    [[nodiscard]] inline bool ProjectSegment(
        const CameraSnapshot& camera, const Point3& from, const Point3& to,
        ScreenPoint& screenFrom, ScreenPoint& screenTo)
    {
        struct ClipPoint { float x, y, w; };
        const auto clipPoint = [&](const Point3& world)
        {
            const float* m = camera.matrix;
            return ClipPoint{
                world.x * m[0] + world.y * m[1] + world.z * m[2] + m[3],
                world.x * m[4] + world.y * m[5] + world.z * m[6] + m[7],
                world.x * m[12] + world.y * m[13] + world.z * m[14] + m[15],
            };
        };
        const auto a = clipPoint(from);
        const auto b = clipPoint(to);
        if (!std::isfinite(a.x) || !std::isfinite(a.y) || !std::isfinite(a.w) ||
            !std::isfinite(b.x) || !std::isfinite(b.y) || !std::isfinite(b.w))
            return false;
        float first = 0.0f;
        float last = 1.0f;
        const auto plane = [&](const float start, const float end)
        {
            if (start < 0.0f && end < 0.0f)
                return false;
            if (start < 0.0f || end < 0.0f)
            {
                const float t = start / (start - end);
                if (start < 0.0f) first = std::max(first, t);
                else last = std::min(last, t);
            }
            return first <= last;
        };
        if (!plane(a.w - 0.001f, b.w - 0.001f) ||
            !plane(a.x + a.w, b.x + b.w) || !plane(a.w - a.x, b.w - b.x) ||
            !plane(a.y + a.w, b.y + b.w) || !plane(a.w - a.y, b.w - b.y))
            return false;
        const auto project = [&](const float t, ScreenPoint& screen)
        {
            const float w = a.w + (b.w - a.w) * t;
            if (w <= 0.0f)
                return false;
            const float x = (a.x + (b.x - a.x) * t) / w;
            const float y = (a.y + (b.y - a.y) * t) / w;
            screen.x = (camera.right - camera.left) * x + camera.right + camera.left - 1.0f;
            screen.y = (camera.top - camera.bottom) * y + camera.top + camera.bottom - 1.0f;
            return std::isfinite(screen.x) && std::isfinite(screen.y);
        };
        return project(first, screenFrom) && project(last, screenTo);
    }
}
