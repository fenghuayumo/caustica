#include <engine/App.h>
#include <engine/EntryPoint.h>
#include <engine/GpuRenderScheduleRegistration.h>
#include <engine/SceneQuery.h>
#include <engine/RenderFrameApi.h>
#include <engine/SceneViewState.h>
#include <engine/SceneScheduleRegistration.h>
#include <engine/SystemLabels.h>

#include <backend/GpuDevice.h>
#include <core/ThreadContext.h>
#include <engine/cmdline_utils.h>
#include <platform/window.h>

#if CAUSTICA_WITH_STREAMLINE
#include <StreamlineIntegration.h>
#endif
#if CAUSTICA_WITH_AFTERMATH
#include <AftermathCrashDump.h>
#endif

#include <chrono>
#include <thread>

namespace caustica
{

void App::onEvent(Event& event)
{
    if (m_eventHandler)
        m_eventHandler(event);
}

void App::queueEvent(std::unique_ptr<Event> event)
{
    std::lock_guard<std::mutex> lock(m_EventQueueMutex);
    m_EventQueue.push_back(std::move(event));
}

void App::processEventQueue()
{
    std::vector<std::unique_ptr<Event>> pending;
    {
        std::lock_guard<std::mutex> lock(m_EventQueueMutex);
        pending.swap(m_EventQueue);
    }
    for (auto& e : pending)
    {
        if (e)
            onEvent(*e);
    }
}

void App::onWindowEvent(Event& event)
{
    onEvent(event);
}

void App::installWindowEventCallback()
{
    if (Window* w = window())
        w->setEventCallback([this](Event& e) { onWindowEvent(e); });
}

static double GetNow(bool headless)
{
    if (!headless)
        return glfwGetTime();
    using Clock = std::chrono::steady_clock;
    static const auto start = Clock::now();
    return std::chrono::duration<double>(Clock::now() - start).count();
}

App::App() = default;

App::App(GpuDevice* gpuDevice, Window* window)
    : m_ExternalGpuDevice(gpuDevice)
    , m_ExternalWindow(window)
{
    bindFrameDriver(gpuDevice);
    if (gpuDevice)
        m_graphicsInitialized = true;
}

App::~App()
{
    if (!m_shutdownCalled)
        shutdown();
}

void App::buildPlugins()
{
    if (m_pluginsBuilt)
        return;

    for (Plugin* plugin : m_pluginRefs)
    {
        if (!plugin)
            continue;

        plugin->build(*this);
        plugin->configureSchedules(*this);
    }

    if (!m_defaultSchedulesRegistered)
        registerDefaultSchedules();

    registerSceneSchedules(*this);
    registerGpuRenderSchedules(*this);

    for (Plugin* plugin : m_pluginRefs)
    {
        if (plugin)
            plugin->configureLateSchedules(*this);
    }

    ensureUpdateTail();
    ensurePostUpdateTail();

    m_pluginsBuilt = true;
}

App& App::addSystem(
    AppSchedule schedule,
    SystemLabel label,
    SystemFn system,
    AppSystemOrdering ordering)
{
    m_schedules.addSystem(schedule, std::move(label), std::move(system), std::move(ordering));
    return *this;
}

void App::runSchedule(AppSchedule schedule, SystemContext& context)
{
    m_schedules.run(schedule, context);
    if (schedule != AppSchedule::render)
    {
        if (ecs::CommandQueue* queue = context.world.getResource<ecs::CommandQueue>(); queue && !queue->empty())
            queue->apply(context.world);
    }
}

void App::registerDefaultSchedules()
{
    if (m_defaultSchedulesRegistered)
        return;

#if CAUSTICA_WITH_STREAMLINE
    addSystem<system_label::StreamlineSimStart>(AppSchedule::First, [](SystemContext& ctx) {
        if (ctx.gpuDevice && !ctx.gpuDevice->m_DeviceParams.headlessDevice)
            StreamlineIntegration::Get().simStart(*ctx.gpuDevice);
    });
#endif

    addSystem<system_label::BeforeFrame>(AppSchedule::First, [](SystemContext& ctx) {
        if (ctx.gpuDevice && ctx.app.beforeFrame)
            ctx.app.beforeFrame(*ctx.gpuDevice, ctx.frameIndex);
    });

    addSystem<system_label::ProcessEventQueue>(AppSchedule::First, [](SystemContext& ctx) {
        ctx.app.processEventQueue();
    });

    addSystem<system_label::NotifyDpiScale>(AppSchedule::preUpdate, [](SystemContext& ctx) {
        if (ctx.runRender && ctx.gpuDevice)
            ctx.app.notifyDpiScaleIfChanged(*ctx.gpuDevice);
    });

    addSystem<system_label::BeforeAnimate>(AppSchedule::preUpdate, [](SystemContext& ctx) {
        if (!ctx.gpuDevice)
            return;

        if (ctx.app.beforeAnimate)
            ctx.app.beforeAnimate(*ctx.gpuDevice, ctx.frameIndex);
        ctx.app.onBeforeAnimate(*ctx.gpuDevice, ctx.frameIndex);
    });

    addSystem<system_label::SetRenderFrameIndex>(AppSchedule::Extract, [](SystemContext& ctx) {
        if (ctx.gpuDevice)
            ctx.gpuDevice->setPreparedRenderFrameIndex(ctx.frameIndex);
    });

    m_defaultSchedulesRegistered = true;
}

void App::ensureUpdateTail()
{
    if (m_updateTailRegistered)
        return;

    addSystemAfter<system_label::SyncRenderThread, system_label::ProcessEventQueue>(
        AppSchedule::First,
        [](SystemContext& ctx) {
            if (!ctx.app.syncRenderThreadCompletedFrames(ctx))
                ctx.abortFrame = true;
        });

    m_updateTailRegistered = true;
}

void App::ensurePostUpdateTail()
{
    if (m_postUpdateTailRegistered)
        return;

    addSystem<system_label::AfterAnimate>(AppSchedule::Last, [](SystemContext& ctx) {
        if (!ctx.runUpdate || !ctx.gpuDevice)
            return;

#if CAUSTICA_WITH_STREAMLINE
        if (ctx.runRender && !ctx.gpuDevice->m_DeviceParams.headlessDevice)
            StreamlineIntegration::Get().simEnd(*ctx.gpuDevice);
#endif
        if (ctx.app.afterAnimate)
            ctx.app.afterAnimate(*ctx.gpuDevice, ctx.frameIndex);
    });

    m_postUpdateTailRegistered = true;
}

void App::runStartupSchedules()
{
    GpuDevice* gpuDevice = device();
    SystemContext context{
        *this,
        m_world,
        gpuDevice,
        0.0f,
        gpuDevice ? gpuDevice->m_FrameIndex : 0u,
        isWindowFocused(),
        isWindowVisible(),
    };
    runSchedule(AppSchedule::Startup, context);
}

bool App::initializeGraphics(const GpuDeviceCreateDesc& desc)
{
    GpuDeviceCreateResult result = GpuDevice::createInitialized(desc);
    if (!result.gpuDevice)
        return false;

    m_GpuDevice = std::move(result.gpuDevice);
    m_Window = std::move(result.window);

    bindFrameDriver(m_GpuDevice.get());
    installWindowEventCallback();
    m_graphicsInitialized = true;
    return true;
}

bool App::initializeGraphics(int argc, const char* const* argv, GpuDeviceCreateDesc& desc)
{
    invokePreGpuDeviceInitHook();

    desc.api = resolveGraphicsAPIFromCommandLine(argc, argv);
    if (desc.headless)
        desc.vsyncEnabled = false;

    return initializeGraphics(desc);
}

bool App::finishStartup()
{
    if (m_started)
        return true;

    if (!m_graphicsInitialized && !m_ExternalGpuDevice)
    {
        error("App::finishStartup requires initializeGraphics or an external GpuDevice");
        return false;
    }

    buildPlugins();

    GpuDevice* gpuDevice = device();
    if (!gpuDevice)
        return false;

    runStartupSchedules();
    syncSwapChain();
    m_started = true;
    return true;
}

void App::syncSwapChain()
{
    GpuDevice* gpuDevice = device();
    if (!gpuDevice || !gpuDevice->getDevice())
        return;

    const BackBufferInfo backBuffer = gpuDevice->getBackBufferInfo();
    notifyBackBufferResizing();
    notifyBackBufferResized(backBuffer.width, backBuffer.height, backBuffer.sampleCount);
}

void App::shutdown()
{
    if (m_shutdownCalled)
        return;

    if (m_pluginsBuilt)
    {
        GpuDevice* gpuDevice = device();
        SystemContext context{
            *this,
            m_world,
            gpuDevice,
            0.0f,
            gpuDevice ? gpuDevice->m_FrameIndex : 0u,
            isWindowFocused(),
            isWindowVisible(),
        };
        runSchedule(AppSchedule::shutdown, context);
    }

    m_renderThread.stop();

    m_started = false;
    m_schedules.clear();
    m_world.clear();
    m_defaultSchedulesRegistered = false;
    m_sceneSchedulesRegistered = false;
    m_gpuRenderSchedulesRegistered = false;
    m_updateTailRegistered = false;
    m_postUpdateTailRegistered = false;

    GpuDevice* gpuDevice = device();
    unbindFrameDriver(gpuDevice);

    if (m_GpuDevice)
    {
        m_GpuDevice->releaseWindowOwnership();
        m_Window.reset();
        m_GpuDevice->shutdown();
        m_GpuDevice.reset();
    }

    m_graphicsInitialized = false;
    m_pluginsBuilt = false;
    m_shutdownCalled = true;
}

GpuDevice* App::getGpuDevice() const { return device(); }
Window* App::getWindow() const { return window(); }

GpuDevice* App::device() const
{
    return m_GpuDevice ? m_GpuDevice.get() : m_ExternalGpuDevice;
}

Window* App::window() const
{
    return m_Window ? m_Window.get() : m_ExternalWindow;
}

void App::bindFrameDriver(GpuDevice* gpuDevice)
{
    if (gpuDevice)
        gpuDevice->setFrameDriver(this);
}

void App::unbindFrameDriver(GpuDevice* gpuDevice)
{
    if (gpuDevice && gpuDevice->getFrameDriver() == static_cast<IGpuFrameDriver*>(this))
        gpuDevice->setFrameDriver(nullptr);
}

void App::notifyBackBufferResizing()
{
    caustica::backBufferResizing(*this);

    if (m_backBufferResizeHandler)
        m_backBufferResizeHandler(true, 0, 0, 0);
}

void App::notifyBackBufferResized(uint32_t width, uint32_t height, uint32_t sampleCount)
{
    onBackBufferResized(width, height, sampleCount);
}

void App::waitForRenderThreadIdle()
{
    if (m_useDedicatedRenderThread && m_renderThread.isRunning())
        m_renderThread.waitForIdle();
}

void App::waitForDedicatedRenderThreadIdle()
{
    waitForRenderThreadIdle();
}

void App::runGpuWorkOnRenderThread(const std::function<void()>& work)
{
    if (!work)
        return;

    if (m_useDedicatedRenderThread && m_renderThread.isRunning())
        m_renderThread.dispatchAndWait(work);
    else
    {
        const ThreadDomainScope renderDomain(ThreadDomain::Render);
        work();
    }
}

void App::requestExit()
{
    m_requestExit = true;
    if (Window* w = window())
        w->setExit(true);
    if (GpuDevice* gpuDevice = device())
    {
        gpuDevice->setShuttingDown(true);
        // Hide immediately so title-bar Close feels instant while the render
        // thread / GPU drain finishes in the background (not a hang/crash).
        if (GLFWwindow* glfwWindow = gpuDevice->getWindow())
            glfwHideWindow(glfwWindow);
    }
}

void App::requestRenderUnfocused()
{
    if (GpuDevice* gpuDevice = device())
        gpuDevice->m_RequestedRenderUnfocused = true;
}

void App::notifyDisplayScaleChanged(float scaleX, float scaleY)
{
    onDisplayScaleChanged(scaleX, scaleY);
}

bool App::isWindowVisible() const
{
    Window* w = window();
    return w ? w->isVisible() : true;
}

bool App::isWindowFocused() const
{
    Window* w = window();
    return w ? w->isFocused() : true;
}

void App::syncDpiScaleFromWindow()
{
    Window* w = window();
    GpuDevice* gpuDevice = device();
    if (!w || !gpuDevice)
        return;

    gpuDevice->m_DPIScaleFactorX = w->getDPIScaleX();
    gpuDevice->m_DPIScaleFactorY = w->getDPIScaleY();
}

void App::syncWindowState()
{
    Window* w = window();
    if (!w)
        return;

    w->onUpdate();
    updateWindowSize();
    syncDpiScaleFromWindow();
}

void App::updateWindowSize()
{
    GpuDevice* gpuDevice = device();
    if (!gpuDevice)
        return;

    // Per-frame dispatchAndWait stalls the UI thread behind a full path-trace
    // frame even when the window size is unchanged — Windows title-bar Close
    // then looks hung (spinning cursor). Only sync when something changed.
    if (m_useDedicatedRenderThread && m_renderThread.isRunning())
    {
        if (!gpuDevice->needsWindowSizeSync())
            return;
        m_renderThread.dispatchAndWait([gpuDevice]() { gpuDevice->updateWindowSize(); });
    }
    else
    {
        gpuDevice->updateWindowSize();
    }
}

void App::animate(double elapsedTime, bool windowIsFocused)
{
    (void)elapsedTime;
    (void)windowIsFocused;
}

void App::render()
{
    onRender();
}

bool App::skipRenderPhase() const
{
    if (tryResource<SceneViewState>())
        return caustica::shouldSkipRender(*this);

    return false;
}

void App::onRender()
{
    GpuDevice* gpuDevice = device();
    if (!gpuDevice || skipRenderPhase())
        return;

    runGpuRenderSchedules(*gpuDevice, gpuDevice->getRenderPhaseFrameIndex());
}

void App::runGpuRenderSchedules(GpuDevice& gpuDevice, uint32_t frameIndex)
{
    SystemContext context{
        *this,
        m_world,
        &gpuDevice,
        0.0f,
        frameIndex,
        true,
        isWindowVisible(),
    };

    runSchedule(AppSchedule::render, context);
}

void App::onBackBufferResizing()
{
    if (m_backBufferResizeHandler)
        m_backBufferResizeHandler(true, 0, 0, 0);
}

void App::onBackBufferResized(uint32_t width, uint32_t height, uint32_t sampleCount)
{
    if (m_backBufferResizeHandler)
        m_backBufferResizeHandler(false, width, height, sampleCount);
}

void App::onDisplayScaleChanged(float scaleX, float scaleY)
{
    if (m_displayScaleHandler)
        m_displayScaleHandler(scaleX, scaleY);
}

bool App::shouldRenderWhenUnfocused() const
{
    if (tryResource<SceneViewState>())
        return caustica::shouldRenderWhenUnfocused(*this);

    return false;
}

void App::finishFrameWithRenderFailure(GpuDevice* gpuDevice, double elapsedTime, double curTime)
{
    gpuDevice->updateAverageFrameTime(elapsedTime);
    gpuDevice->m_PreviousFrameTimestamp = curTime;
    ++gpuDevice->m_FrameIndex;
}

void App::notifyDpiScaleIfChanged(GpuDevice& gpuDevice)
{
    if (gpuDevice.m_PrevDPIScaleFactorX != gpuDevice.m_DPIScaleFactorX ||
        gpuDevice.m_PrevDPIScaleFactorY != gpuDevice.m_DPIScaleFactorY)
    {
        notifyDisplayScaleChanged(gpuDevice.m_DPIScaleFactorX, gpuDevice.m_DPIScaleFactorY);
        gpuDevice.m_PrevDPIScaleFactorX = gpuDevice.m_DPIScaleFactorX;
        gpuDevice.m_PrevDPIScaleFactorY = gpuDevice.m_DPIScaleFactorY;
    }
    gpuDevice.m_RequestedRenderUnfocused = false;
}

bool App::syncRenderThreadCompletedFrames(SystemContext& context)
{
    if (m_useDedicatedRenderThread && !m_renderThread.isRunning())
        m_renderThread.start();

    if (!m_useDedicatedRenderThread || !m_renderThread.isRunning())
        return true;

    GpuDevice* gpuDevice = context.gpuDevice;
    while (auto completed = m_renderThread.consumeCompletedFrame())
    {
        if (!completed->success)
        {
            if (gpuDevice)
                finishFrameWithRenderFailure(gpuDevice, completed->elapsedTime, completed->curTime);
            return false;
        }
    }

    return true;
}

bool App::dispatchScheduledRender(SystemContext& context)
{
    GpuDevice* gpuDevice = context.gpuDevice;
    if (!gpuDevice)
        return true;

    const uint32_t renderFrameIndex = context.frameIndex;
    const double elapsedTime = context.elapsedTime;
    const double curTime = context.currentTime;

#if CAUSTICA_WITH_STREAMLINE
    void* slFrameToken = nullptr;
    if (!gpuDevice->m_DeviceParams.headlessDevice)
        slFrameToken = StreamlineIntegration::Get().getFrameTokenForRender();
#endif

    if (m_useDedicatedRenderThread)
    {
        m_renderThread.dispatch([this, gpuDevice, elapsedTime, curTime, renderFrameIndex
#if CAUSTICA_WITH_STREAMLINE
            , slFrameToken
#endif
        ]() {
#if CAUSTICA_WITH_STREAMLINE
            StreamlineIntegration::RenderFrameTokenScope slFrameScope(slFrameToken);
#endif
            const bool ok = executeRenderPhase(gpuDevice, elapsedTime, curTime, renderFrameIndex);
            if (!ok)
            {
                // A second frame may already be queued. Stop it before it tries to
                // create command lists on a removed device; the logic thread will
                // consume the failed completion and terminate the frame loop.
                gpuDevice->setShuttingDown(true);
            }
            m_renderThread.notifyFrameCompleted({ ok, elapsedTime, curTime });
        });
        return true;
    }

#if CAUSTICA_WITH_STREAMLINE
    StreamlineIntegration::RenderFrameTokenScope slFrameScope(slFrameToken);
#endif
    if (!executeRenderPhase(gpuDevice, elapsedTime, curTime, renderFrameIndex))
    {
        finishFrameWithRenderFailure(gpuDevice, elapsedTime, curTime);
        return false;
    }

    return true;
}

void App::finalizeFrameTiming(GpuDevice& gpuDevice, double elapsedTime, double curTime)
{
    std::this_thread::sleep_for(std::chrono::milliseconds(0));
    gpuDevice.getDevice()->runGarbageCollection();
    gpuDevice.updateAverageFrameTime(elapsedTime);
    gpuDevice.m_PreviousFrameTimestamp = curTime;
    ++gpuDevice.m_FrameIndex;
}

bool App::executeRenderPhase(GpuDevice* gpuDevice, double elapsedTime, double curTime, uint32_t frameIndex)
{
    const ThreadDomainScope renderDomain(ThreadDomain::Render);

    if (frameIndex == 0 && gpuDevice->m_SkipRenderOnFirstFrame)
        return true;

    // Window close: finish the frame token quickly without path-tracing / RTPSO work.
    if (gpuDevice->isShuttingDown() || m_requestExit)
        return true;

    gpuDevice->setRenderPhaseFrameIndex(frameIndex);

    if (!gpuDevice->beginFrame())
        return true;

    uint32_t fi = frameIndex;
    if (gpuDevice->m_SkipRenderOnFirstFrame)
        --fi;
#if CAUSTICA_WITH_STREAMLINE
    if (!gpuDevice->m_DeviceParams.headlessDevice)
        StreamlineIntegration::Get().renderStart(*gpuDevice);
#endif
    if (beforeRender)
        beforeRender(*gpuDevice, fi);
    render();
    if (afterRender)
        afterRender(*gpuDevice, fi);
#if CAUSTICA_WITH_STREAMLINE
    if (!gpuDevice->m_DeviceParams.headlessDevice)
    {
        StreamlineIntegration::Get().renderEnd(*gpuDevice);
        StreamlineIntegration::Get().presentStart(*gpuDevice);
    }
#endif
    if (beforePresent)
        beforePresent(*gpuDevice, fi);
    const bool ok = gpuDevice->present();
    if (afterPresent)
        afterPresent(*gpuDevice, fi);
#if CAUSTICA_WITH_STREAMLINE
    if (!gpuDevice->m_DeviceParams.headlessDevice)
        StreamlineIntegration::Get().presentEnd(*gpuDevice);
#endif
    return ok;
}

bool App::runFrame(std::optional<double> elapsedTimeOverride)
{
    GpuDevice* gpuDevice = device();
    if (!gpuDevice)
        return false;

    const double curTime = GetNow(gpuDevice->m_DeviceParams.headlessDevice);
    const double elapsedTime = elapsedTimeOverride.value_or(curTime - gpuDevice->m_PreviousFrameTimestamp);

    const bool windowVisible = isWindowVisible();
    const bool windowFocused = isWindowFocused();
    const bool wantsRender = windowFocused || shouldRenderWhenUnfocused() || gpuDevice->m_RequestedRenderUnfocused;

    SystemContext scheduleContext{
        *this,
        m_world,
        gpuDevice,
        float(elapsedTime),
        gpuDevice->m_FrameIndex,
        windowFocused,
        windowVisible,
    };
    scheduleContext.elapsedTime = elapsedTime;
    scheduleContext.currentTime = curTime;
    scheduleContext.runUpdate = windowVisible;
    scheduleContext.runRender = windowVisible && wantsRender && !skipRenderPhase();

    if (scheduleContext.runUpdate)
    {
        scheduleContext.windowFocused = scheduleContext.runRender;
        runSchedule(AppSchedule::First, scheduleContext);
        if (scheduleContext.abortFrame)
            return false;
        runSchedule(AppSchedule::preUpdate, scheduleContext);
        if (scheduleContext.abortFrame)
            return false;
        runSchedule(AppSchedule::update, scheduleContext);
        if (scheduleContext.abortFrame)
            return false;
        runSchedule(AppSchedule::PostUpdate, scheduleContext);
    }

    if (scheduleContext.runRender)
    {
        // Snapshot slots use the logic frame index. A no-render gap can advance that
        // index onto a slot still owned by an older render frame, so drain once before
        // resuming Extract. Consecutive render frames keep the normal two-frame pipeline.
        if (m_renderSkippedSinceLastSubmission)
        {
            waitForRenderThreadIdle();
            m_renderSkippedSinceLastSubmission = false;
        }
        runSchedule(AppSchedule::Extract, scheduleContext);
        if (!dispatchScheduledRender(scheduleContext))
            scheduleContext.abortFrame = true;
        if (scheduleContext.abortFrame)
            return false;
        runSchedule(AppSchedule::postRender, scheduleContext);
    }
    else
    {
        m_renderSkippedSinceLastSubmission = true;
    }

    runSchedule(AppSchedule::Last, scheduleContext);
    finalizeFrameTiming(*gpuDevice, elapsedTime, curTime);
    return true;
}

void App::run()
{
    GpuDevice* gpuDevice = device();
    if (!gpuDevice)
    {
        error("App::run requires an initialized GpuDevice");
        return;
    }

    if (!m_started)
    {
        error("App::run requires finishStartup");
        return;
    }

    bindFrameDriver(gpuDevice);
    gpuDevice->m_PreviousFrameTimestamp = GetNow(gpuDevice->m_DeviceParams.headlessDevice);

    if (m_useDedicatedRenderThread)
        m_renderThread.start();

#if CAUSTICA_WITH_AFTERMATH
    bool dumpingCrash = false;
#endif

    Window* w = window();
    if (!w)
    {
        if (!gpuDevice->m_DeviceParams.headlessDevice)
        {
            error("App::run requires a Window");
            return;
        }

        constexpr double kHeadlessFrameTimeSeconds = 1.0 / 60.0;
        while (!m_requestExit)
        {
            if (!runFrame(kHeadlessFrameTimeSeconds))
            {
#if CAUSTICA_WITH_AFTERMATH
                dumpingCrash = true;
#endif
                break;
            }
        }

        gpuDevice->setShuttingDown(true);

        if (m_useDedicatedRenderThread)
            m_renderThread.waitForIdle();

        bool ok = gpuDevice->getDevice()->waitForIdle();
#if CAUSTICA_WITH_AFTERMATH
        dumpingCrash |= !ok;
        if (dumpingCrash && gpuDevice->m_DeviceParams.enableAftermath)
            AftermathCrashDump::waitForCrashDump();
#else
        (void)ok;
#endif

        shutdown();
        return;
    }

    installWindowEventCallback();

    while (!w->getExit())
    {
        w->onUpdate();
        // Close was requested during event polling -- skip another full path-trace
        // frame so exit does not block on a multi-second render.
        if (w->getExit() || m_requestExit)
            break;

        updateWindowSize();
        syncDpiScaleFromWindow();
        if (!runFrame())
        {
#if CAUSTICA_WITH_AFTERMATH
            dumpingCrash = true;
#endif
            break;
        }
    }

    // Signal in-flight RT work to skip long RTPSO / shader rebuilds before we drain.
    gpuDevice->setShuttingDown(true);

    if (m_useDedicatedRenderThread)
        m_renderThread.waitForIdle();

    bool ok = gpuDevice->getDevice()->waitForIdle();
#if CAUSTICA_WITH_AFTERMATH
    dumpingCrash |= !ok;
    if (dumpingCrash && gpuDevice->m_DeviceParams.enableAftermath)
        AftermathCrashDump::waitForCrashDump();
#else
    (void)ok;
#endif

    shutdown();
}

bool App::frame()
{
    return stepFrame();
}

bool App::stepFrame()
{
    return stepFrame(-1.0);
}

bool App::stepFrame(double dt)
{
    syncWindowState();
    return dt >= 0.0 ? runFrame(std::max(0.0, dt)) : runFrame();
}

} // namespace caustica
