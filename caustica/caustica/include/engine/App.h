#pragma once

#include <backend/GpuDevice.h>
#include <backend/GpuFrameDriver.h>
#include <engine/AppSchedules.h>
#include <engine/Engine.h>
#include <engine/ISubsystem.h>
#include <engine/Plugin.h>
#include <engine/RenderThread.h>
#include <events/event.h>

#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <type_traits>
#include <utility>
#include <vector>

namespace caustica
{

class GpuDevice;
class Window;

// Plugin-driven application: Engine, window/GPU, and frame loop.
//
//   App app;
//   app.addPlugin<DefaultPlugins>(sceneConfig);
//   app.initializeGraphics(argc, argv, desc);
//   app.finishStartup();
//   app.run();
class App : public IGpuFrameDriver
{
public:
    App();
    App(GpuDevice* gpuDevice, Window* window = nullptr);
    ~App();

    template<typename T, typename... Args>
    App& addPlugin(Args&&... args)
    {
        static_assert(std::is_base_of_v<Plugin, T>, "T must derive from Plugin");
        auto plugin = std::make_unique<T>(std::forward<Args>(args)...);
        m_pluginRefs.push_back(plugin.get());
        m_ownedPlugins.push_back(std::move(plugin));
        m_pluginsBuilt = false;
        return *this;
    }

    App& addPlugin(Plugin& plugin)
    {
        m_pluginRefs.push_back(&plugin);
        m_pluginsBuilt = false;
        return *this;
    }

    template<typename T, typename... Args>
    T& emplaceSubsystem(Args&&... args)
    {
        static_assert(std::is_base_of_v<ISubsystem, T>, "T must derive from ISubsystem");
        auto subsystem = std::make_unique<T>(std::forward<Args>(args)...);
        T& ref = *subsystem;
        m_engine.addSubsystem(std::move(subsystem));
        return ref;
    }

    void buildPlugins();

    App& addSystem(AppSchedule schedule, std::string name, AppSystemFn system);
    void runSchedule(AppSchedule schedule, AppScheduleContext& context);
    [[nodiscard]] AppSchedules& schedules() { return m_schedules; }
    [[nodiscard]] const AppSchedules& schedules() const { return m_schedules; }

    bool initializeGraphics(const GpuDeviceCreateDesc& desc);
    bool initializeGraphics(int argc, const char* const* argv, GpuDeviceCreateDesc& desc);
    bool finishStartup();
    void syncSwapChain();

    void shutdown();

    GpuDevice* getGpuDevice() const;
    Window* getWindow() const;

    Engine& engine() { return m_engine; }
    const Engine& engine() const { return m_engine; }

    void run();

    bool frame();
    bool stepFrame();
    bool stepFrame(double fixedElapsedTimeSeconds);

    bool runFrame(std::optional<double> elapsedTimeOverride = std::nullopt);
    void animate(double elapsedTime, bool windowIsFocused);
    void render();

    void setUseDedicatedRenderThread(bool enabled) { m_useDedicatedRenderThread = enabled; }
    [[nodiscard]] bool useDedicatedRenderThread() const { return m_useDedicatedRenderThread; }
    [[nodiscard]] RenderThread& renderThread() { return m_renderThread; }
    [[nodiscard]] const RenderThread& renderThread() const { return m_renderThread; }

    void waitForDedicatedRenderThreadIdle();
    void runGpuWorkOnRenderThread(const std::function<void()>& work);
    void requestExit();

    using FrameCallback = std::function<void(GpuDevice&, uint32_t frameIndex)>;
    FrameCallback beforeFrame;
    FrameCallback beforeAnimate;
    FrameCallback afterAnimate;
    FrameCallback beforeRender;
    FrameCallback afterRender;
    FrameCallback beforePresent;
    FrameCallback afterPresent;

    using EventHandler = std::function<void(Event&)>;
    void setEventHandler(EventHandler handler) { m_eventHandler = std::move(handler); }

    using DisplayScaleHandler = std::function<void(float, float)>;
    void setDisplayScaleHandler(DisplayScaleHandler handler) { m_displayScaleHandler = std::move(handler); }

    // IGpuFrameDriver
    void notifyBackBufferResizing() override;
    void notifyBackBufferResized(uint32_t width, uint32_t height, uint32_t sampleCount) override;
    void waitForRenderThreadIdle() override;

    void notifyDisplayScaleChanged(float scaleX, float scaleY);

    virtual void onEvent(Event& event);

    void queueEvent(std::unique_ptr<Event> event);
    void processEventQueue();

protected:
    virtual void onBeginFrame(GpuDevice& gpuDevice);
    virtual void onBeforeAnimate(GpuDevice& gpuDevice, uint32_t frameIndex) {}
    virtual bool skipRenderPhase() const;

    void bindFrameDriver(GpuDevice* dm);
    void unbindFrameDriver(GpuDevice* dm);

    virtual void onUpdate(float elapsedTimeSeconds, bool windowFocused);
    virtual void onPrepareRenderScene(GpuDevice& gpuDevice);
    virtual void onRender();
    virtual void onBackBufferResizing();
    virtual void onBackBufferResized(uint32_t width, uint32_t height, uint32_t sampleCount);
    virtual void onDisplayScaleChanged(float scaleX, float scaleY);
    virtual bool shouldRenderWhenUnfocused() const;

    void onWindowEvent(Event& event);
    void installWindowEventCallback();

    Engine m_engine;

    std::unique_ptr<GpuDevice> m_GpuDevice;
    std::unique_ptr<Window> m_Window;

    bool m_shutdownCalled = false;
    bool m_pluginsBuilt = false;
    bool m_engineInitialized = false;
    bool m_defaultSchedulesBuilt = false;

    AppSchedules m_schedules;

private:
    void syncWindowState();
    void updateWindowSize();
    void syncDpiScaleFromWindow();
    bool isWindowVisible() const;
    bool isWindowFocused() const;

    bool executeRenderPhase(GpuDevice* gpuDevice, double elapsedTime, double curTime, uint32_t frameIndex);
    void finishFrameWithRenderFailure(GpuDevice* gpuDevice, double elapsedTime, double curTime);
    void shutdownEngine();
    void ensureDefaultSchedules();
    void runStartupSchedules();

    GpuDevice* device() const;
    Window* window() const;

    GpuDevice* m_ExternalGpuDevice = nullptr;
    Window* m_ExternalWindow = nullptr;

    std::mutex m_EventQueueMutex;
    std::vector<std::unique_ptr<Event>> m_EventQueue;

    std::vector<Plugin*> m_pluginRefs;
    std::vector<std::unique_ptr<Plugin>> m_ownedPlugins;

    EventHandler m_eventHandler;
    DisplayScaleHandler m_displayScaleHandler;

    RenderThread m_renderThread;
    bool m_useDedicatedRenderThread = true;
    bool m_requestExit = false;
};

} // namespace caustica
