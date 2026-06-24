#pragma once

#include <functional>
#include <memory>
#include <optional>

namespace caustica {

class GpuDevice;
class Window;

// =============================================================================
// Application — Engine layer: owns the app lifecycle (DIVSHOT-style).
//
// Derived editor apps (SampleBaseApp / AdvancedSample) override init() and
// shutdown() to create window, device, and render passes. run() drives the
// message loop and calls shutdown() when the window closes.
//
// RenderSession uses the non-owning constructor for manual stepFrame() control.
// =============================================================================
class Application
{
public:
    Application();
    Application(GpuDevice* dm, Window* window = nullptr);
    virtual ~Application();

    // Editor bootstrap (override in SampleBaseApp).
    virtual bool init(int argc, const char* const* argv);

    // Tear down editor resources (override in SampleBaseApp). Called from run().
    virtual void shutdown();

    GpuDevice* getGpuDevice() const;
    Window*    getWindow() const;

    // --- Message loop (DIVSHOT: run -> while frame -> shutdown) ---
    void run();

    // --- Single frame ---
    bool frame();
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

protected:
    std::unique_ptr<GpuDevice> m_GpuDevice;
    std::unique_ptr<Window>    m_Window;

    bool m_shutdownCalled = false;

private:
    void syncWindowState();
    void updateWindowSize();
    void syncDpiScaleFromWindow();
    bool shouldRenderUnfocused() const;
    bool isWindowVisible() const;
    bool isWindowFocused() const;

    GpuDevice* device() const;
    Window*    window() const;

    GpuDevice* m_ExternalGpuDevice = nullptr;
    Window*    m_ExternalWindow    = nullptr;
};

} // namespace caustica
