#include <engine/App.h>
#include <engine/AppObservers.h>
#include <engine/AppResources.h>
#include <engine/internal/GpuRenderScheduleRegistration.h>
#include <engine/internal/WorldRendererAccess.h>
#include <engine/SceneQuery.h>
#include <engine/RenderFrameApi.h>
#include <engine/SceneViewState.h>
#include <engine/SceneScheduleRegistration.h>
#include <engine/SystemLabels.h>
#include <engine/SystemSets.h>
#include <engine/Time.h>

#include <backend/GpuDevice.h>
#include <backend/GpuSurface.h>
#include <core/ThreadContext.h>

#include <core/task/TaskRuntime.h>
#include <platform/window.h>
#include <render/AppDiagnostics.h>
#include <render/core/PathTracerSettings.h>
#include <render/WorldRenderer.h>
#include <render/passes/postProcess/ToneMappingPasses.h>

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

void App::addEventObserver(std::function<void(Event&)> handler)
{
    if (auto* observers = tryResource<EventObservers>())
        observers->handlers.push_back(std::move(handler));
}

void App::dispatchEvent(Event& event)
{
    if (event.getEventType() == EventType::WindowClose)
        requestExit();

    if (auto* observers = tryResource<EventObservers>())
    {
        for (auto& handler : observers->handlers)
        {
            if (handler)
                handler(event);
        }
    }
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
            dispatchEvent(*e);
    }
}

void App::installWindowEventCallback()
{
    if (Window* w = window())
        w->setEventCallback([this](Event& e) { dispatchEvent(e); });
}

static double GetNow()
{
    using Clock = std::chrono::steady_clock;
    static const auto start = Clock::now();
    return std::chrono::duration<double>(Clock::now() - start).count();
}

App::App()
    : m_surfaceHook(*this)
{
    emplaceResource<EventObservers>();
}

App::App(GpuDevice* gpuDevice, Window* window, GpuSurface* surface)
    : App()
{
    m_device = gpuDevice;
    m_window = window;
    bindSurface(surface);
    installWindowEventCallback();
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
    return addSystemWithAccess(
        schedule,
        std::move(label),
        std::move(system),
        ecs::SystemAccess{},
        std::move(ordering));
}

App& App::addSystemWithAccess(
    AppSchedule schedule,
    SystemLabel label,
    SystemFn system,
    ecs::SystemAccess access,
    AppSystemOrdering ordering)
{
    // Default: gameplay / host systems on `update` join Simulation unless tagged otherwise.
    if (schedule == AppSchedule::update && !ordering.set.valid())
        ordering.inSet<system_set::Simulation>();

    m_schedules.addSystem(
        schedule, std::move(label), std::move(system), std::move(access), std::move(ordering));
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

    // Default SystemSet edges (Bevy-style groupings).
    m_schedules.configureSetAfterOthers(
        AppSchedule::PostUpdate, systemLabel<system_set::TransformPropagate>());

#if CAUSTICA_WITH_STREAMLINE
    addSystem<system_label::StreamlineSimStart>(AppSchedule::First, [](SystemContext& ctx) {
        if (ctx.gpuDevice && !ctx.gpuDevice->isHeadless())
            StreamlineIntegration::Get().simStart(*ctx.gpuDevice);
    });
#endif

    addSystem<system_label::ProcessEventQueue>(AppSchedule::First, [](SystemContext& ctx) {
        ctx.app.processEventQueue();
    });

    addSystem<system_label::NotifyDpiScale>(AppSchedule::preUpdate, [](SystemContext& ctx) {
        if (ctx.runRender)
            ctx.app.notifyDpiScaleIfChanged();
    });

    // Ordering fence for editor / host systems that run just before simulation.
    addSystem<system_label::PrepareSimulation>(AppSchedule::preUpdate, [](SystemContext&) {});

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

    addSystem<system_label::EndSimulation>(AppSchedule::Last, [](SystemContext& ctx) {
        if (!ctx.runUpdate || !ctx.gpuDevice)
            return;

#if CAUSTICA_WITH_STREAMLINE
        if (ctx.runRender && !ctx.gpuDevice->isHeadless())
            StreamlineIntegration::Get().simEnd(*ctx.gpuDevice);
#endif
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
        gpuDevice ? gpuDevice->getFrameIndex() : 0u,
        isWindowFocused(),
        isWindowVisible(),
    };
    runSchedule(AppSchedule::Startup, context);
}

bool App::finishStartup()
{
    if (m_started)
        return true;

    if (!device())
    {
        error("App::finishStartup requires a GpuDevice");
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
    GpuSurface* gpuSurface = surface();
    if (!gpuSurface)
        return;

    gpuSurface->handleResizing();
    gpuSurface->handleResized();
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
            gpuDevice ? gpuDevice->getFrameIndex() : 0u,
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

    unbindSurface();
    m_device = nullptr;
    m_window = nullptr;

    m_pluginsBuilt = false;
    m_shutdownCalled = true;
}

GpuDevice* App::getGpuDevice() const { return device(); }
Window* App::getWindow() const { return window(); }
GpuSurface* App::getSurface() const { return surface(); }

void App::bindSurface(GpuSurface* gpuSurface)
{
    unbindSurface();
    m_surface = gpuSurface;
    if (m_surface)
        m_surface->addObserver(m_surfaceHook);
}

void App::unbindSurface()
{
    if (m_surface)
        m_surface->removeObserver(m_surfaceHook);
    m_surface = nullptr;
}

void App::handleSurfaceResizing()
{
    caustica::backBufferResizing(*this);
}

void App::handleSurfaceResized(uint32_t, uint32_t, uint32_t)
{
}

void App::waitForRenderThreadIdle()
{
    if (m_useDedicatedRenderThread && m_renderThread.isRunning())
        m_renderThread.waitForIdle();
}

void App::enqueueRenderCommandAndWaitImpl(std::function<void()> command)
{
    if (!command)
        return;

    if (m_useDedicatedRenderThread && m_renderThread.isRunning())
    {
        m_renderThread.dispatchAndWait(std::move(command));
        return;
    }

    // --syncRender: Logic thread pumps the sole Affinity::Render domain queue.
    const ThreadDomainScope renderDomain(ThreadDomain::Render);
    task::TaskDesc desc;
    desc.name = "SyncRender.Command";
    desc.priority = task::Priority::Critical;
    desc.affinity = task::Affinity::Render;
    desc.body = std::move(command);
    task::TaskHandle handle = task::launch(std::move(desc));
    while (!task::poll(handle))
        task::pumpRender();
}

void App::enqueueRenderCommandImpl(std::function<void()> command)
{
    if (!command)
        return;

    if (m_useDedicatedRenderThread && m_renderThread.isRunning())
    {
        m_renderThread.dispatch(std::move(command));
        return;
    }

    // --syncRender: same Affinity::Render queue as LoadSession / dedicated RT.
    const ThreadDomainScope renderDomain(ThreadDomain::Render);
    task::TaskDesc desc;
    desc.name = "SyncRender.Command";
    desc.priority = task::Priority::High;
    desc.affinity = task::Affinity::Render;
    desc.body = std::move(command);
    (void)task::launch(std::move(desc));
    task::pumpRender();
}

void App::requestExit()
{
    // requestExit() may be called by Logic, Render, or a worker. Only publish
    // thread-safe state here; GLFW/window operations are consumed by run() on
    // the main thread.
    m_requestExit.store(true, std::memory_order_release);
    if (GpuDevice* gpuDevice = device())
        gpuDevice->setShuttingDown(true);
}

void App::requestRenderUnfocused()
{
    if (GpuDevice* gpuDevice = device())
        gpuDevice->requestRenderUnfocused();
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
    if (GpuSurface* gpuSurface = surface())
        gpuSurface->syncDpiFromWindow();
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
    GpuSurface* gpuSurface = surface();
    if (!gpuSurface)
        return;

    if (m_useDedicatedRenderThread && m_renderThread.isRunning())
    {
        if (!gpuSurface->needsWindowSizeSync())
            return;
        enqueueRenderCommandImpl([gpuSurface]() { gpuSurface->updateWindowSize(); });
    }
    else
    {
        gpuSurface->updateWindowSize();
    }
}

bool App::skipRenderPhase() const
{
    // Exclusive teardown and high-pressure GPU streaming skip full frame submission.
    if (const SceneViewState* vs = tryResource<SceneViewState>())
    {
        if (vs->sceneGpuSuspended.load(std::memory_order_acquire))
            return true;
    }

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


bool App::shouldRenderWhenUnfocused() const
{
    if (tryResource<SceneViewState>())
        return caustica::shouldRenderWhenUnfocused(*this);

    return false;
}

void App::finishFrameWithRenderFailure(GpuDevice* gpuDevice, double elapsedTime, double curTime)
{
    gpuDevice->advanceFrameClock(elapsedTime, curTime);
}

void App::notifyDpiScaleIfChanged()
{
    if (GpuSurface* gpuSurface = surface())
    {
        float scaleX = 1.f;
        float scaleY = 1.f;
        (void)gpuSurface->takeDpiScaleChange(scaleX, scaleY);
    }
    if (GpuDevice* gpuDevice = device())
        gpuDevice->clearRenderUnfocusedRequest();
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
    const PathTracerSettings* frameSettings = caustica::settings(*this);
    // Imported animation poses are sampled on the logic thread. Until all
    // skinning frame resources are isolated per in-flight frame, do not let
    // Logic(N+1) advance animation state while Render(N) consumes it.
    const bool synchronizeAnimationFrame = isSceneLoaded(*this)
        && frameSettings
        && frameSettings->RealtimeMode
        && frameSettings->EnableAnimations;
#if CAUSTICA_WITH_STREAMLINE
    void* slFrameToken = nullptr;
    if (!gpuDevice->isHeadless())
        slFrameToken = StreamlineIntegration::Get().getFrameTokenForRender();
#endif

    if (m_useDedicatedRenderThread)
    {
        std::function<void()> renderWork = [this, gpuDevice, elapsedTime, curTime, renderFrameIndex
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
        };
        if (synchronizeAnimationFrame)
            m_renderThread.dispatchAndWait(std::move(renderWork));
        else
            m_renderThread.dispatch(std::move(renderWork));
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
    gpuDevice.advanceFrameClock(elapsedTime, curTime);
}

bool App::executeRenderPhase(GpuDevice* gpuDevice, double elapsedTime, double curTime, uint32_t frameIndex)
{
    const ThreadDomainScope renderDomain(ThreadDomain::Render);
    render::FrameTelemetry* telemetry = nullptr;
    if (auto* diagnostics = tryResource<render::AppDiagnostics>())
        telemetry = &diagnostics->frameTelemetry;
    render::ScopedFrameCpuTimer renderTimer(telemetry, frameIndex, render::FrameCpuStage::Render);
    // In --syncRender mode the logic thread must pump Affinity::Render work.
    // A dedicated render thread is already inside task::pumpRender() here: pumping
    // again would recursively execute queued frames and overlap beginFrame/present.
    if (!m_useDedicatedRenderThread || !m_renderThread.isRunning())
        task::pumpRender();

    if (gpuDevice->isShuttingDown() || m_requestExit.load(std::memory_order_acquire))
        return true;

    gpuDevice->setRenderPhaseFrameIndex(frameIndex);

    bool beganFrame = false;
    {
        render::ScopedFrameCpuTimer acquireTimer(
            telemetry, frameIndex, render::FrameCpuStage::Acquire);
        beganFrame = m_surface ? m_surface->acquireFrame() : false;
    }
    if (!beganFrame)
    {
        caustica::rhi::Device* rhiDevice = gpuDevice->getDevice();
        if (rhiDevice && !rhiDevice->isDeviceHealthy())
        {
            caustica::error("Render phase stopped because the GPU device is no longer healthy");
            gpuDevice->setShuttingDown(true);
            return false;
        }
        return true;
    }

    if (auto* renderer = worldRenderer(*this))
    {
        if (auto* toneMappingPass = renderer->getToneMappingPass())
            toneMappingPass->advanceFrame(static_cast<float>(elapsedTime));
    }

#if CAUSTICA_WITH_STREAMLINE
    if (!gpuDevice->isHeadless())
        StreamlineIntegration::Get().renderStart(*gpuDevice);
#endif
    onRender();

    if (caustica::rhi::Device* rhiDevice = gpuDevice->getDevice();
        rhiDevice && !rhiDevice->isDeviceHealthy())
    {
        caustica::error("Render phase aborted before present after a GPU device failure");
        gpuDevice->setShuttingDown(true);
        return false;
    }
#if CAUSTICA_WITH_STREAMLINE
    if (!gpuDevice->isHeadless())
    {
        StreamlineIntegration::Get().renderEnd(*gpuDevice);
        StreamlineIntegration::Get().presentStart(*gpuDevice);
    }
#endif
    bool ok = false;
    {
        render::ScopedFrameCpuTimer presentTimer(
            telemetry, frameIndex, render::FrameCpuStage::Present);
        ok = m_surface ? m_surface->presentFrame() : false;
    }
    if (telemetry)
    {
        const PresentRuntimeInfo present = gpuDevice->getPresentRuntimeInfo();
        telemetry->setPresentStats(
            frameIndex,
            present.headless,
            present.requestedVsync,
            present.activeVsync,
            present.windowed,
            present.tearingSupported,
            present.tearingActive,
            present.backBufferCount);
    }
#if CAUSTICA_WITH_STREAMLINE
    if (!gpuDevice->isHeadless())
        StreamlineIntegration::Get().presentEnd(*gpuDevice);
#endif
    if (caustica::rhi::Device* rhiDevice = gpuDevice->getDevice())
        rhiDevice->runGarbageCollection();
    return ok;
}

bool App::runFrame(std::optional<double> elapsedTimeOverride)
{
    GpuDevice* gpuDevice = device();
    if (!gpuDevice)
        return false;

    const double curTime = GetNow();
    const double elapsedTime = elapsedTimeOverride.value_or(curTime - gpuDevice->getPreviousFrameTimestamp());

    const bool windowVisible = isWindowVisible();
    const bool windowFocused = isWindowFocused();
    const bool wantsRender = windowFocused || shouldRenderWhenUnfocused() || gpuDevice->wantsRenderUnfocused();

    SystemContext scheduleContext{
        *this,
        m_world,
        gpuDevice,
        float(elapsedTime),
        gpuDevice->getFrameIndex(),
        windowFocused,
        windowVisible,
    };
    scheduleContext.elapsedTime = elapsedTime;
    scheduleContext.currentTime = curTime;
    scheduleContext.runUpdate = windowVisible;

    if (Time* time = tryResource<Time>())
    {
        time->deltaSeconds = float(elapsedTime);
        time->elapsedSeconds += elapsedTime;
        time->averageFrameSeconds = gpuDevice->getAverageFrameTimeSeconds();
        time->simulationActive = scheduleContext.windowFocused;
        ++time->frameCount;
    }

    scheduleContext.runRender = windowVisible && wantsRender && !skipRenderPhase();

    render::FrameTelemetry* telemetry = nullptr;
    if (auto* diagnostics = tryResource<render::AppDiagnostics>())
    {
        telemetry = &diagnostics->frameTelemetry;
        telemetry->beginFrame(scheduleContext.frameIndex);
    }

    if (scheduleContext.runUpdate)
    {
        render::ScopedFrameCpuTimer logicTimer(
            telemetry, scheduleContext.frameIndex, render::FrameCpuStage::Logic);
        // Affinity::Logic domain pump (ADR 0001) — before First/update systems.
        task::pumpLogic();

        // Collapse focus to "actually presenting" so camera/anim pause when not
        // drawing — but keep systems pumping while sceneGpuSuspended skips render
        // (async scene import joins via updateLoading).
        scheduleContext.windowFocused = scheduleContext.runRender || skipRenderPhase();
        if (Time* time = tryResource<Time>())
            time->simulationActive = scheduleContext.windowFocused;
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
        // THREADING: Logic↔RT wait — ADR 0002 S5 remaining (rare; skip-render gap only).
        if (m_renderSkippedSinceLastSubmission)
        {
            waitForRenderThreadIdle();
            m_renderSkippedSinceLastSubmission = false;
        }
        {
            render::ScopedFrameCpuTimer extractTimer(
                telemetry, scheduleContext.frameIndex, render::FrameCpuStage::Extract);
            runSchedule(AppSchedule::Extract, scheduleContext);
        }
        {
            render::ScopedFrameCpuTimer queueTimer(
                telemetry, scheduleContext.frameIndex, render::FrameCpuStage::FrameQueueWait);
            if (!dispatchScheduledRender(scheduleContext))
                scheduleContext.abortFrame = true;
        }
        if (scheduleContext.abortFrame)
            return false;
        runSchedule(AppSchedule::postRender, scheduleContext);
    }
    else
    {
        // A suspended scene skips the normal render phase while its exclusive
        // GPU load transaction is active. In sync-render/headless mode that
        // phase is also the usual Render-domain pump, so keep pumping here or
        // LoadSession.StreamStep can never execute and the scene stays suspended.
        if (!m_useDedicatedRenderThread || !m_renderThread.isRunning())
        {
            const ThreadDomainScope renderDomain(ThreadDomain::Render);
            task::pumpRender();
        }
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

    bindSurface(m_surface);
    gpuDevice->seedFrameClock(GetNow());

    if (m_useDedicatedRenderThread)
        m_renderThread.start();

#if CAUSTICA_WITH_AFTERMATH
    bool dumpingCrash = false;
#endif

    Window* w = window();
    if (!w)
    {
        if (!gpuDevice->isHeadless())
        {
            error("App::run requires a Window");
            return;
        }

        constexpr double kHeadlessFrameTimeSeconds = 1.0 / 60.0;
        while (!m_requestExit.load(std::memory_order_acquire))
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

        // THREADING: sync-point, shutdown — ADR 0002 allowed.
        if (m_useDedicatedRenderThread)
            m_renderThread.waitForIdle();

        bool ok = gpuDevice->getDevice()->waitForIdle();
#if CAUSTICA_WITH_AFTERMATH
        dumpingCrash |= !ok;
        if (dumpingCrash && gpuDevice->isAftermathEnabled())
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
        if (m_requestExit.load(std::memory_order_acquire))
        {
            // Window state and GLFW APIs are main-thread owned. requestExit()
            // only publishes the request so Render/worker callers never race
            // GlfwWindow::m_ExitRequested or call GLFW from the wrong thread.
            w->setExit(true);
            w->hide();
            break;
        }
        if (w->getExit())
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

    // THREADING: sync-point, shutdown — ADR 0002 allowed.
    if (m_useDedicatedRenderThread)
        m_renderThread.waitForIdle();

    bool ok = gpuDevice->getDevice()->waitForIdle();
#if CAUSTICA_WITH_AFTERMATH
    dumpingCrash |= !ok;
    if (dumpingCrash && gpuDevice->isAftermathEnabled())
        AftermathCrashDump::waitForCrashDump();
#else
    (void)ok;
#endif

    shutdown();
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
