#pragma once

#include <backend/GpuDevice.h>
#include <backend/GpuFrameDriver.h>
#include <engine/AppSchedules.h>
#include <engine/AppResources.h>
#include <engine/Plugin.h>
#include <engine/RenderThread.h>
#include <events/event.h>

#include <cstdint>

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

// Plugin-driven application: window/GPU and frame loop.
//
// Lifecycle:
//   app.addPlugin<DefaultPlugins>(sceneConfig);
//   app.initializeGraphics(argc, argv, desc);
//   app.finishStartup();
//   app.run();  // main loop, then shutdown
//
// Or use runApp(app, startup) where startup registers plugins and calls
// initializeGraphics + finishStartup before returning.
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
    T& emplaceResource(Args&&... args)
    {
        return m_resources.emplace<T>(std::forward<Args>(args)...);
    }

    void buildPlugins();
    void registerDefaultSchedules();
    void ensureUpdateTail();
    void ensurePostUpdateTail();

    [[nodiscard]] bool sceneSessionSchedulesRegistered() const { return m_sceneSessionSchedulesRegistered; }
    void markSceneSessionSchedulesRegistered() { m_sceneSessionSchedulesRegistered = true; }

    [[nodiscard]] bool gpuRenderSchedulesRegistered() const { return m_gpuRenderSchedulesRegistered; }
    void markGpuRenderSchedulesRegistered() { m_gpuRenderSchedulesRegistered = true; }

    template<typename T>
    T& insertResource(T value)
    {
        return m_resources.insert(std::move(value));
    }

    template<typename T>
    T& insertResourceRef(T& resource)
    {
        return m_resources.insertRef(resource);
    }

    template<typename T>
    const T& insertResourceRef(const T& resource)
    {
        return m_resources.insertRef(resource);
    }

    template<typename T>
    [[nodiscard]] T& resource()
    {
        return m_resources.get<T>();
    }

    template<typename T>
    [[nodiscard]] const T& resource() const
    {
        return m_resources.get<T>();
    }

    template<typename T>
    [[nodiscard]] T* tryResource()
    {
        return m_resources.tryGet<T>();
    }

    template<typename T>
    [[nodiscard]] const T* tryResource() const
    {
        return m_resources.tryGet<T>();
    }

    [[nodiscard]] AppResources& resources() { return m_resources; }
    [[nodiscard]] const AppResources& resources() const { return m_resources; }

    App& addSystem(
        AppSchedule schedule,
        std::string name,
        AppSystemFn system,
        AppSystemOrdering ordering = {});
    App& addSystemBefore(
        AppSchedule schedule,
        std::string name,
        std::string before,
        AppSystemFn system);
    App& addSystemAfter(
        AppSchedule schedule,
        std::string name,
        std::string after,
        AppSystemFn system);
    void runSchedule(AppSchedule schedule, AppScheduleContext& context);
    [[nodiscard]] AppSchedules& schedules() { return m_schedules; }
    [[nodiscard]] const AppSchedules& schedules() const { return m_schedules; }

    bool initializeGraphics(const GpuDeviceCreateDesc& desc);
    bool initializeGraphics(int argc, const char* const* argv, GpuDeviceCreateDesc& desc);
    bool finishStartup();

    [[nodiscard]] bool isGraphicsInitialized() const { return m_graphicsInitialized; }
    [[nodiscard]] bool isStarted() const { return m_started; }

    void syncSwapChain();

    void shutdown();

    GpuDevice* getGpuDevice() const;
    Window* getWindow() const;

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
    void requestRenderUnfocused();

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

    using BackBufferResizeHandler = std::function<void(bool resizing, uint32_t width, uint32_t height, uint32_t sampleCount)>;
    void setBackBufferResizeHandler(BackBufferResizeHandler handler) { m_backBufferResizeHandler = std::move(handler); }

    // IGpuFrameDriver
    void notifyBackBufferResizing() override;
    void notifyBackBufferResized(uint32_t width, uint32_t height, uint32_t sampleCount) override;
    void waitForRenderThreadIdle() override;

    void notifyDisplayScaleChanged(float scaleX, float scaleY);

    virtual void onEvent(Event& event);

    void queueEvent(std::unique_ptr<Event> event);
    void processEventQueue();

protected:
    virtual void onBeforeAnimate(GpuDevice& gpuDevice, uint32_t frameIndex) {}

    void bindFrameDriver(GpuDevice* dm);
    void unbindFrameDriver(GpuDevice* dm);

    void onRender();
    void onBackBufferResizing();
    void onBackBufferResized(uint32_t width, uint32_t height, uint32_t sampleCount);
    void onDisplayScaleChanged(float scaleX, float scaleY);

    [[nodiscard]] bool skipRenderPhase() const;
    [[nodiscard]] bool shouldRenderWhenUnfocused() const;

    void onWindowEvent(Event& event);
    void installWindowEventCallback();

    std::unique_ptr<GpuDevice> m_GpuDevice;
    std::unique_ptr<Window> m_Window;

    bool m_shutdownCalled = false;
    bool m_pluginsBuilt = false;
    bool m_graphicsInitialized = false;
    bool m_started = false;
    bool m_defaultSchedulesRegistered = false;
    bool m_sceneSessionSchedulesRegistered = false;
    bool m_gpuRenderSchedulesRegistered = false;
    bool m_updateTailRegistered = false;
    bool m_postUpdateTailRegistered = false;

    AppSchedules m_schedules;
    AppResources m_resources;

private:
    void syncWindowState();
    void updateWindowSize();
    void syncDpiScaleFromWindow();
    bool isWindowVisible() const;
    bool isWindowFocused() const;

    bool executeRenderPhase(GpuDevice* gpuDevice, double elapsedTime, double curTime, uint32_t frameIndex);
    void finishFrameWithRenderFailure(GpuDevice* gpuDevice, double elapsedTime, double curTime);
    void runStartupSchedules();

    void notifyDpiScaleIfChanged(GpuDevice& gpuDevice);
    bool syncRenderThreadCompletedFrames(AppScheduleContext& context);
    bool dispatchScheduledRender(AppScheduleContext& context);
    void finalizeFrameTiming(GpuDevice& gpuDevice, double elapsedTime, double curTime);
    void runGpuRenderSchedules(GpuDevice& gpuDevice, uint32_t frameIndex);

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
    BackBufferResizeHandler m_backBufferResizeHandler;

    RenderThread m_renderThread;
    bool m_useDedicatedRenderThread = true;
    bool m_requestExit = false;
};

} // namespace caustica
