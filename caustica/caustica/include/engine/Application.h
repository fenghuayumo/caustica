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
// Derived editor (EditorApplication) calls startup() then run(); shutdown() tears down passes.
//
// RenderSession uses the non-owning constructor for manual stepFrame() control.
// =============================================================================
class Application
{
public:
    Application();
    Application(GpuDevice* dm, Window* window = nullptr);
    virtual ~Application();

    // Override in EditorApplication for editor bootstrap.
    virtual bool init(int argc, const char* const* argv);

    // Override in EditorApplication. Called from run() when the window closes.
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
