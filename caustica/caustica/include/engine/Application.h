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
// Derived editor (EditorApplication) calls startup() then run(); shutdown() tears down.
// Frame update/render is driven through onUpdate/onRender overrides, not a pass list.
//
// RenderSession uses the non-owning constructor for manual stepFrame() control.
// =============================================================================
class Application
{
public:
    Application();
    Application(GpuDevice* dm, Window* window = nullptr);
    virtual ~Application();

    virtual bool init(int argc, const char* const* argv);
    virtual void shutdown();

    GpuDevice* getGpuDevice() const;
    Window*    getWindow() const;

    void run();

    bool frame();
    bool stepFrame();
    bool stepFrame(double fixedElapsedTimeSeconds);

    bool runFrame(std::optional<double> elapsedTimeOverride = std::nullopt);
    void animate(double elapsedTime, bool windowIsFocused);
    void render();

    using FrameCallback = std::function<void(GpuDevice&, uint32_t frameIndex)>;
    FrameCallback beforeFrame;
    FrameCallback beforeAnimate;
    FrameCallback afterAnimate;
    FrameCallback beforeRender;
    FrameCallback afterRender;
    FrameCallback beforePresent;
    FrameCallback afterPresent;

    // GpuDevice back-buffer lifecycle callbacks (called when this app is the frame driver).
    void notifyBackBufferResizing();
    void notifyBackBufferResized(uint32_t width, uint32_t height, uint32_t sampleCount);
    void notifyDisplayScaleChanged(float scaleX, float scaleY);

protected:
    void bindFrameDriver(GpuDevice* dm);
    void unbindFrameDriver(GpuDevice* dm);

    virtual void onUpdate(float elapsedTimeSeconds, bool windowFocused) {}
    virtual void onRender() {}
    virtual void onBackBufferResizing() {}
    virtual void onBackBufferResized(uint32_t width, uint32_t height, uint32_t sampleCount) {}
    virtual void onDisplayScaleChanged(float scaleX, float scaleY) {}
    virtual bool shouldRenderWhenUnfocused() const { return false; }

    std::unique_ptr<GpuDevice> m_GpuDevice;
    std::unique_ptr<Window>    m_Window;

    bool m_shutdownCalled = false;

private:
    void syncWindowState();
    void updateWindowSize();
    void syncDpiScaleFromWindow();
    bool isWindowVisible() const;
    bool isWindowFocused() const;

    GpuDevice* device() const;
    Window*    window() const;

    GpuDevice* m_ExternalGpuDevice = nullptr;
    Window*    m_ExternalWindow    = nullptr;
};

} // namespace caustica
