#pragma once

#include <functional>
#include <optional>

namespace caustica {

class DeviceManager;
class Window;

// =============================================================================
// Application — Engine layer: message loop, frame pacing, animation, rendering.
//
// Extracted from DeviceManager. Uses friend access to DeviceManager for
// GPU device/swapchain operations until GpuDevice and SwapChain are ready.
// =============================================================================
class Application
{
public:
    Application(DeviceManager* dm, Window* window = nullptr);
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

    // --- Callbacks (mirror DeviceManager's for migration) ---
    using FrameCallback = std::function<void(DeviceManager&, uint32_t frameIndex)>;
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

    DeviceManager* m_DM;
    Window*        m_Window;
};

} // namespace caustica
