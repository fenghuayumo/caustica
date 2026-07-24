#pragma once

#include <backend/GpuDevice.h>
#include <backend/GpuFrameDriver.h>
#include <engine/AppSchedules.h>
#include <engine/Plugin.h>
#include <engine/RenderThread.h>
#include <ecs/World.h>
#include <events/event.h>

#include <cstdint>

#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <type_traits>
#include <tuple>
#include <utility>
#include <vector>

namespace caustica
{

class GpuDevice;
class Window;

namespace detail
{
template<typename T>
struct SystemCallableTraits;

template<typename C, typename R, typename... Args>
struct SystemCallableTraits<R(C::*)(Args...)>
{
    using ArgsTuple = std::tuple<Args...>;
};

template<typename C, typename R, typename... Args>
struct SystemCallableTraits<R(C::*)(Args...) const>
{
    using ArgsTuple = std::tuple<Args...>;
};

template<typename T>
struct SystemParameter
{
    static_assert(!std::is_same_v<T, T>, "Unsupported typed system parameter");
};

template<typename T>
struct SystemParameter<Res<T>>
{
    static Res<T> make(SystemContext& context) { return Res<T>(context.res<T>()); }
};

template<typename T>
struct SystemParameter<ResMut<T>>
{
    static ResMut<T> make(SystemContext& context) { return ResMut<T>(context.resMut<T>()); }
};

template<>
struct SystemParameter<Commands>
{
    static Commands make(SystemContext& context) { return Commands(context.commands()); }
};

template<>
struct SystemParameter<SystemContext>
{
    static SystemContext& make(SystemContext& context) { return context; }
};

template<typename Parameter>
decltype(auto) makeSystemParameter(SystemContext& context)
{
    using RawParameter = std::remove_cvref_t<Parameter>;
    return SystemParameter<RawParameter>::make(context);
}

template<typename F, typename Tuple, std::size_t... Indices>
void invokeTypedSystem(F& system, SystemContext& context, std::index_sequence<Indices...>)
{
    std::tuple<decltype(makeSystemParameter<std::tuple_element_t<Indices, Tuple>>(context))...> parameters(
        makeSystemParameter<std::tuple_element_t<Indices, Tuple>>(context)...);
    std::apply([&](auto&... parameters) { std::invoke(system, parameters...); }, parameters);
}

template<typename F>
SystemFn makeTypedSystem(F&& system)
{
    using Callable = std::remove_cvref_t<F>;
    using Arguments = typename SystemCallableTraits<decltype(&Callable::operator())>::ArgsTuple;
    return [system = std::forward<F>(system)](SystemContext& context) mutable {
        invokeTypedSystem<Callable, Arguments>(
            system, context, std::make_index_sequence<std::tuple_size_v<Arguments>>{});
    };
}
} // namespace detail

// Plugin-driven application: window/GPU and frame loop.
//
// Prefer EngineApp::create for new apps (Bevy-like one-liner):
//   auto engine = EngineApp::create({ .scene = "Kitchen/kitchen.json" });
//   engine->app().addSystem<MySimLabel>(AppSchedule::update, ...);
//   engine->run();
//
// Low-level lifecycle (advanced):
//   app.addPlugins(DefaultPlugins{sceneConfig});
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

    template<typename G, typename... Args>
    App& addPlugins(Args&&... args)
    {
        static_assert(std::is_base_of_v<PluginGroup, G>, "G must derive from PluginGroup");
        G group(std::forward<Args>(args)...);
        group.build(*this);
        return *this;
    }

    App& addPlugins(PluginGroup& group)
    {
        group.build(*this);
        return *this;
    }

    App& addPlugins(PluginGroup&& group)
    {
        group.build(*this);
        return *this;
    }

    template<typename T, typename... Args>
    T& emplaceResource(Args&&... args)
    {
        return m_world.insertResource<T>(std::forward<Args>(args)...);
    }

    void buildPlugins();
    void registerDefaultSchedules();
    void ensureUpdateTail();
    void ensurePostUpdateTail();

    [[nodiscard]] bool sceneSchedulesRegistered() const { return m_sceneSchedulesRegistered; }
    void markSceneSchedulesRegistered() { m_sceneSchedulesRegistered = true; }

    [[nodiscard]] bool gpuRenderSchedulesRegistered() const { return m_gpuRenderSchedulesRegistered; }
    void markGpuRenderSchedulesRegistered() { m_gpuRenderSchedulesRegistered = true; }

    template<typename T>
    T& insertResource(T value)
    {
        return m_world.insertResourceValue<T>(std::move(value));
    }

    template<typename T>
    T& insertResourceRef(T& resource)
    {
        return m_world.insertResourceRef(resource);
    }

    template<typename T>
    const T& insertResourceRef(const T& resource)
    {
        return m_world.insertResourceRef(resource);
    }

    template<typename T>
    [[nodiscard]] T& resource()
    {
        return m_world.resource<T>();
    }

    template<typename T>
    [[nodiscard]] const T& resource() const
    {
        return m_world.resource<T>();
    }

    template<typename T>
    [[nodiscard]] T* tryResource()
    {
        return m_world.getResource<T>();
    }

    template<typename T>
    [[nodiscard]] const T* tryResource() const
    {
        return m_world.getResource<T>();
    }

    [[nodiscard]] ecs::World& world() { return m_world; }
    [[nodiscard]] const ecs::World& world() const { return m_world; }

    App& addSystem(
        AppSchedule schedule,
        SystemLabel label,
        SystemFn system,
        AppSystemOrdering ordering = {});

    template<typename Label>
    App& addSystem(AppSchedule schedule, SystemFn system, AppSystemOrdering ordering = {})
    {
        return addSystem(schedule, systemLabel<Label>(), std::move(system), std::move(ordering));
    }

    template<typename Label, class F>
    App& addSystem(AppSchedule schedule, F&& system, AppSystemOrdering ordering = {})
        requires (!std::is_convertible_v<std::decay_t<F>, SystemFn>)
    {
        return addSystem(
            schedule,
            systemLabel<Label>(),
            detail::makeTypedSystem(std::forward<F>(system)),
            std::move(ordering));
    }

    template<typename Label, typename AfterLabel>
    App& addSystemAfter(AppSchedule schedule, SystemFn system)
    {
        return addSystem<Label>(schedule, std::move(system), AppSystemOrdering{}.runAfter<AfterLabel>());
    }

    template<typename Label, typename AfterLabel, class F>
    App& addSystemAfter(AppSchedule schedule, F&& system)
        requires (!std::is_convertible_v<std::decay_t<F>, SystemFn>)
    {
        return addSystemAfter<Label, AfterLabel>(
            schedule, detail::makeTypedSystem(std::forward<F>(system)));
    }

    template<typename Label, typename BeforeLabel>
    App& addSystemBefore(AppSchedule schedule, SystemFn system)
    {
        return addSystem<Label>(schedule, std::move(system), AppSystemOrdering{}.runBefore<BeforeLabel>());
    }

    template<typename Label, typename BeforeLabel, class F>
    App& addSystemBefore(AppSchedule schedule, F&& system)
        requires (!std::is_convertible_v<std::decay_t<F>, SystemFn>)
    {
        return addSystemBefore<Label, BeforeLabel>(
            schedule, detail::makeTypedSystem(std::forward<F>(system)));
    }
    void runSchedule(AppSchedule schedule, SystemContext& context);
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
    // Blocks until the work finishes on the render thread (resize / exclusive setup).
    void runGpuWorkOnRenderThread(const std::function<void()>& work);
    // Fire-and-forget RT work. Blocks only when the frame queue is full (same as dispatch).
    void enqueueGpuWorkOnRenderThread(const std::function<void()>& work);
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
    bool m_sceneSchedulesRegistered = false;
    bool m_gpuRenderSchedulesRegistered = false;
    bool m_updateTailRegistered = false;
    bool m_postUpdateTailRegistered = false;

    AppSchedules m_schedules;
    ecs::World m_world;

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
    bool syncRenderThreadCompletedFrames(SystemContext& context);
    bool dispatchScheduledRender(SystemContext& context);
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
    bool m_renderSkippedSinceLastSubmission = false;
    bool m_requestExit = false;
};

} // namespace caustica
