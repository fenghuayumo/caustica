#include <engine/App.h>
#include <engine/EntryPoint.h>
#include <engine/SubsystemScheduleRegistration.h>

#include <backend/GpuDevice.h>
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

    registerSubsystemSchedules(*this);
    ensurePreUpdateTail();
    ensurePostUpdateTail();

    m_pluginsBuilt = true;
}

App& App::addSystem(AppSchedule schedule, std::string name, AppSystemFn system)
{
    m_schedules.addSystem(schedule, std::move(name), std::move(system));
    return *this;
}

void App::runSchedule(AppSchedule schedule, AppScheduleContext& context)
{
    m_schedules.run(schedule, context);
}

void App::registerDefaultSchedules()
{
    if (m_defaultSchedulesRegistered)
        return;

#if CAUSTICA_WITH_STREAMLINE
    addSystem(AppSchedule::PreUpdate, "StreamlineSimStart", [](AppScheduleContext& ctx) {
        if (ctx.gpuDevice && !ctx.gpuDevice->m_DeviceParams.headlessDevice)
            StreamlineIntegration::Get().SimStart(*ctx.gpuDevice);
    });
#endif

    addSystem(AppSchedule::PreUpdate, "BeforeFrame", [](AppScheduleContext& ctx) {
        if (ctx.gpuDevice && ctx.app.beforeFrame)
            ctx.app.beforeFrame(*ctx.gpuDevice, ctx.frameIndex);
    });

    addSystem(AppSchedule::PreUpdate, "ProcessEventQueue", [](AppScheduleContext& ctx) {
        ctx.app.processEventQueue();
    });

    addSystem(AppSchedule::Update, "NotifyDpiScale", [](AppScheduleContext& ctx) {
        if (ctx.runRender && ctx.gpuDevice)
            ctx.app.notifyDpiScaleIfChanged(*ctx.gpuDevice);
    });

    addSystem(AppSchedule::Update, "BeforeAnimate", [](AppScheduleContext& ctx) {
        if (!ctx.gpuDevice)
            return;

        if (ctx.app.beforeAnimate)
            ctx.app.beforeAnimate(*ctx.gpuDevice, ctx.frameIndex);
        ctx.app.onBeforeAnimate(*ctx.gpuDevice, ctx.frameIndex);
    });

    addSystem(AppSchedule::PreRender, "SetRenderFrameIndex", [](AppScheduleContext& ctx) {
        if (ctx.gpuDevice)
            ctx.gpuDevice->SetPreparedRenderFrameIndex(ctx.frameIndex);
    });

    addSystem(AppSchedule::Render, "ExecuteGpuRender", [](AppScheduleContext& ctx) {
        if (!ctx.app.dispatchScheduledRender(ctx))
            ctx.abortFrame = true;
    });

    m_defaultSchedulesRegistered = true;
}

void App::ensurePreUpdateTail()
{
    if (m_preUpdateTailRegistered)
        return;

    addSystem(AppSchedule::PreUpdate, "SyncRenderThread", [](AppScheduleContext& ctx) {
        if (!ctx.app.syncRenderThreadCompletedFrames(ctx))
            ctx.abortFrame = true;
    });

    m_preUpdateTailRegistered = true;
}

void App::ensurePostUpdateTail()
{
    if (m_postUpdateTailRegistered)
        return;

    addSystem(AppSchedule::PostUpdate, "AfterAnimate", [](AppScheduleContext& ctx) {
        if (!ctx.gpuDevice)
            return;

#if CAUSTICA_WITH_STREAMLINE
        if (ctx.runRender && !ctx.gpuDevice->m_DeviceParams.headlessDevice)
            StreamlineIntegration::Get().SimEnd(*ctx.gpuDevice);
#endif
        if (ctx.app.afterAnimate)
            ctx.app.afterAnimate(*ctx.gpuDevice, ctx.frameIndex);
    });

    m_postUpdateTailRegistered = true;
}

void App::runStartupSchedules()
{
    GpuDevice* gpuDevice = device();
    AppScheduleContext context{
        *this,
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
    GpuDeviceCreateResult result = GpuDevice::CreateInitialized(desc);
    if (!result.gpuDevice)
        return false;

    m_GpuDevice = std::move(result.gpuDevice);
    m_Window = std::move(result.window);

    bindFrameDriver(m_GpuDevice.get());
    installWindowEventCallback();
    return true;
}

bool App::initializeGraphics(int argc, const char* const* argv, GpuDeviceCreateDesc& desc)
{
    InvokePreGpuDeviceInitHook();

    desc.api = ResolveGraphicsAPIFromCommandLine(argc, argv);
    if (desc.headless)
        desc.vsyncEnabled = false;

    return initializeGraphics(desc);
}

bool App::finishStartup()
{
    if (m_engineInitialized)
        return true;

    buildPlugins();

    GpuDevice* gpuDevice = device();
    if (!gpuDevice)
        return false;

    if (!m_engine.initialize(EngineInitContext{
            .gpuDevice = gpuDevice,
            .window = window(),
            .app = this,
        }))
    {
        return false;
    }

    syncSwapChain();
    runStartupSchedules();
    m_engineInitialized = true;
    return true;
}

void App::syncSwapChain()
{
    GpuDevice* gpuDevice = device();
    if (gpuDevice)
        Engine::syncSwapChain(*gpuDevice, *this);
}

void App::shutdownEngine()
{
    if (!m_engineInitialized)
        return;

    m_engine.shutdown();
    m_engineInitialized = false;
}

void App::shutdown()
{
    if (m_shutdownCalled)
        return;

    m_renderThread.stop();

    shutdownEngine();
    m_schedules.clear();
    m_defaultSchedulesRegistered = false;
    m_subsystemSchedulesRegistered = false;
    m_preUpdateTailRegistered = false;
    m_postUpdateTailRegistered = false;

    GpuDevice* gpuDevice = device();
    unbindFrameDriver(gpuDevice);

    if (m_GpuDevice)
    {
        m_GpuDevice->ReleaseWindowOwnership();
        m_Window.reset();
        m_GpuDevice->Shutdown();
        m_GpuDevice.reset();
    }

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
    onBackBufferResizing();
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
        work();
}

void App::requestExit()
{
    m_requestExit = true;
    if (Window* w = window())
        w->setExit(true);
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

    if (m_useDedicatedRenderThread && m_renderThread.isRunning())
        m_renderThread.dispatchAndWait([gpuDevice]() { gpuDevice->UpdateWindowSize(); });
    else
        gpuDevice->UpdateWindowSize();
}

void App::animate(double elapsedTime, bool windowIsFocused)
{
    onUpdate(float(elapsedTime), windowIsFocused);
}

void App::render()
{
    onRender();
}

void App::onBeginFrame(GpuDevice& gpuDevice)
{
    m_engine.onBeginFrame(gpuDevice);
}

bool App::skipRenderPhase() const
{
    return m_engine.skipRenderPhase();
}

void App::onUpdate(float elapsedTimeSeconds, bool windowFocused)
{
    m_engine.onUpdate(elapsedTimeSeconds, windowFocused);
}

void App::onPrepareRenderScene(GpuDevice& gpuDevice)
{
    m_engine.onPrepareRenderScene(gpuDevice);
}

void App::onRender()
{
    GpuDevice* gpuDevice = device();
    if (!gpuDevice || m_engine.skipRenderPhase())
        return;

    runGpuSubsystemSchedules(*gpuDevice, gpuDevice->GetRenderPhaseFrameIndex());
}

void App::runGpuSubsystemSchedules(GpuDevice& gpuDevice, uint32_t frameIndex)
{
    AppScheduleContext context{
        *this,
        &gpuDevice,
        0.0f,
        frameIndex,
        true,
        isWindowVisible(),
    };

    runSchedule(AppSchedule::RenderScene, context);
    runSchedule(AppSchedule::RenderFinalize, context);
}

void App::onBackBufferResizing()
{
    m_engine.onBackBufferResizing();
}

void App::onBackBufferResized(uint32_t width, uint32_t height, uint32_t sampleCount)
{
    m_engine.onBackBufferResized(width, height, sampleCount);
}

void App::onDisplayScaleChanged(float scaleX, float scaleY)
{
    if (m_displayScaleHandler)
        m_displayScaleHandler(scaleX, scaleY);
}

bool App::shouldRenderWhenUnfocused() const
{
    return m_engine.shouldRenderWhenUnfocused();
}

void App::finishFrameWithRenderFailure(GpuDevice* gpuDevice, double elapsedTime, double curTime)
{
    gpuDevice->UpdateAverageFrameTime(elapsedTime);
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

bool App::syncRenderThreadCompletedFrames(AppScheduleContext& context)
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

bool App::dispatchScheduledRender(AppScheduleContext& context)
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
    gpuDevice.GetDevice()->runGarbageCollection();
    gpuDevice.UpdateAverageFrameTime(elapsedTime);
    gpuDevice.m_PreviousFrameTimestamp = curTime;
    ++gpuDevice.m_FrameIndex;
}

bool App::executeRenderPhase(GpuDevice* gpuDevice, double elapsedTime, double curTime, uint32_t frameIndex)
{
    if (frameIndex == 0 && gpuDevice->m_SkipRenderOnFirstFrame)
        return true;

    gpuDevice->SetRenderPhaseFrameIndex(frameIndex);

    if (!gpuDevice->BeginFrame())
        return true;

    uint32_t fi = frameIndex;
    if (gpuDevice->m_SkipRenderOnFirstFrame)
        --fi;
#if CAUSTICA_WITH_STREAMLINE
    if (!gpuDevice->m_DeviceParams.headlessDevice)
        StreamlineIntegration::Get().RenderStart(*gpuDevice);
#endif
    if (beforeRender)
        beforeRender(*gpuDevice, fi);
    render();
    if (afterRender)
        afterRender(*gpuDevice, fi);
#if CAUSTICA_WITH_STREAMLINE
    if (!gpuDevice->m_DeviceParams.headlessDevice)
    {
        StreamlineIntegration::Get().RenderEnd(*gpuDevice);
        StreamlineIntegration::Get().PresentStart(*gpuDevice);
    }
#endif
    if (beforePresent)
        beforePresent(*gpuDevice, fi);
    const bool ok = gpuDevice->Present();
    if (afterPresent)
        afterPresent(*gpuDevice, fi);
#if CAUSTICA_WITH_STREAMLINE
    if (!gpuDevice->m_DeviceParams.headlessDevice)
        StreamlineIntegration::Get().PresentEnd(*gpuDevice);
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

    AppScheduleContext scheduleContext{
        *this,
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

    runSchedule(AppSchedule::PreUpdate, scheduleContext);
    if (scheduleContext.abortFrame)
        return false;

    if (scheduleContext.runUpdate)
    {
        scheduleContext.windowFocused = scheduleContext.runRender;
        runSchedule(AppSchedule::Update, scheduleContext);
        runSchedule(AppSchedule::PostUpdate, scheduleContext);
    }

    if (scheduleContext.runRender)
    {
        runSchedule(AppSchedule::PreRender, scheduleContext);
        runSchedule(AppSchedule::Render, scheduleContext);
        if (scheduleContext.abortFrame)
            return false;
        runSchedule(AppSchedule::PostRender, scheduleContext);
    }

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

        if (m_useDedicatedRenderThread)
            m_renderThread.waitForIdle();

        bool ok = gpuDevice->GetDevice()->waitForIdle();
#if CAUSTICA_WITH_AFTERMATH
        dumpingCrash |= !ok;
        if (dumpingCrash && gpuDevice->m_DeviceParams.enableAftermath)
            AftermathCrashDump::WaitForCrashDump();
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

    if (m_useDedicatedRenderThread)
        m_renderThread.waitForIdle();

    bool ok = gpuDevice->GetDevice()->waitForIdle();
#if CAUSTICA_WITH_AFTERMATH
    dumpingCrash |= !ok;
    if (dumpingCrash && gpuDevice->m_DeviceParams.enableAftermath)
        AftermathCrashDump::WaitForCrashDump();
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
