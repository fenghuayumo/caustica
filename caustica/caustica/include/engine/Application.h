#pragma once

#include <functional>
#include <optional>

namespace caustica {

class GpuDevice;
class Window;

// =============================================================================
// Application — Engine layer: message loop, frame pacing, animation, rendering.
//
// Extracted from GpuDevice. Uses friend access to GpuDevice for
// GPU device/swapchain operations until GpuDevice and SwapChain are ready.
// =============================================================================
class Application
{
public:
    Application(GpuDevice* dm, Window* window = nullptr);
    ~Application() = default;

    // --- Message loop ---
    void run();

    // --- Single frame ---
    bool stepFrame();
    bool stepFrame(double fixedElapsedTimeSeconds);

    // --- Per-frame logic ---
    bool runFrame(std::optional<double> elapsedTimeOverride = std::nullopt);
    void animate(double elapsedTime, bool windowIsFocused);
    void render();

    // --- Callbacks (mirror GpuDevice's for migration) ---
    using FrameCallback = std::function<void(GpuDevice&, uint32_t frameIndex)>;
    FrameCallback beforeFrame;
    FrameCallback beforeAnimate;
    FrameCallback afterAnimate;
    FrameCallback beforeRender;
    FrameCallback afterRender;
    FrameCallback beforePresent;
    FrameCallback afterPresent;

private:
    void syncWindowState();
    void updateWindowSize();
    bool shouldRenderUnfocused() const;

    GpuDevice* m_DM;
    Window*        m_Window;
};

} // namespace caustica
