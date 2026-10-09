#pragma once

// The overlay renders directly into Skyrim's existing D3D11 swap chain. It
// has no secondary window and starts disabled; Insert toggles drawing.
namespace Skyrim::Overlay
{
    [[nodiscard]] bool Install();
    void Shutdown();
    [[nodiscard]] bool IsEnabled();
}
