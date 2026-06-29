#pragma once

#include <backend/GpuDevice.h>
#include <backend/GpuFrameDriver.h>
#include <events/event.h>

#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

namespace caustica {

class GpuDevice;
class Window;

// =============================================================================
// Application — Engine layer: owns the app lifecycle (DIVSHOT-style).
//
// Derived editor (EditorApplication) calls startup() then run(); shutdown() tears down.
// Frame update/render is driven through onUpdate/onRender overrides, not a pass list.
//
// The event system flows: GLFW → GlfwWindow → EventCallback → Application::onEvent()
// Override onEvent() to receive all input/window events. Use EventDispatcher for
// type-safe dispatch to individual handlers.
//
// RenderSession uses the non-owning constructor for manual stepFrame() control.
//
// Application executables implement createApplication() (see EntryPoint.h).
// =============================================================================
class Application : public IGpuFrameDriver
{
public:
    Application();
    Application(GpuDevice* dm, Window* window = nullptr);
    virtual ~Application();

    virtual bool init(int argc, const char* const* argv);
    virtual void shutdown();

    // Creates GpuDevice, optional Window, and swap chain; wires frame driver + window events.
    bool initializeGraphics(const GpuDeviceCreateDesc& desc);
    bool initializeGraphics(int argc, const char* const* argv, GpuDeviceCreateDesc& desc);

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

    // IGpuFrameDriver — invoked by GpuDevice during swap-chain resize.
    void notifyBackBufferResizing() override;
    void notifyBackBufferResized(uint32_t width, uint32_t height, uint32_t sampleCount) override;

    void notifyDisplayScaleChanged(float scaleX, float scaleY);

    // --- Event system (DIVSHOT-style) ---

    // Override to handle all events. Base implementation queues events; derived
    // classes should call the base if they don't fully handle an event.
    virtual void onEvent(Event& event);

    // Thread-safe event queue for deferred processing (e.g. between frame phases).
    void queueEvent(std::unique_ptr<Event> event);
    void processEventQueue();

protected:
    void bindFrameDriver(GpuDevice* dm);
    void unbindFrameDriver(GpuDevice* dm);

    virtual void onUpdate(float elapsedTimeSeconds, bool windowFocused) {}
    virtual void onRender() {}
    virtual void onBackBufferResizing() {}
    virtual void onBackBufferResized(uint32_t width, uint32_t height, uint32_t sampleCount) {}
    virtual void onDisplayScaleChanged(float scaleX, float scaleY) {}
    virtual bool shouldRenderWhenUnfocused() const { return false; }

    // Called by the window event callback; wires GlfwWindow → Application.
    void onWindowEvent(Event& event);
    void installWindowEventCallback();

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

    std::mutex m_EventQueueMutex;
    std::vector<std::unique_ptr<Event>> m_EventQueue;
};

} // namespace caustica
