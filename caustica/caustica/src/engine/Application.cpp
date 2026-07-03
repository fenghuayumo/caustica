#include "engine/Application.h"
#include "engine/EntryPoint.h"
#include "backend/GpuDevice.h"
#include "engine/cmdline_utils.h"
#include "platform/window.h"
#if CAUSTICA_WITH_STREAMLINE
#include <StreamlineIntegration.h>
#endif
#if CAUSTICA_WITH_AFTERMATH
#include "AftermathCrashDump.h"
#endif

#include <chrono>
#include <thread>

namespace caustica {

// ---------------------------------------------------------------------------
// Event system
// ---------------------------------------------------------------------------

void Application::onEvent(Event& event)
{
    // Base implementation is a no-op. Derived classes override this to
    // dispatch events to subsystems via EventDispatcher.
}

void Application::queueEvent(std::unique_ptr<Event> event)
{
    std::lock_guard<std::mutex> lock(m_EventQueueMutex);
    m_EventQueue.push_back(std::move(event));
}

void Application::processEventQueue()
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

void Application::onWindowEvent(Event& event)
{
    onEvent(event);
}

void Application::installWindowEventCallback()
{
    Window* w = window();
    if (w)
    {
        w->setEventCallback([this](Event& e) { this->onWindowEvent(e); });
    }
}

static double GetNow(bool headless)
{
    if (!headless) return glfwGetTime();
    using Clock = std::chrono::steady_clock;
    static const auto start = Clock::now();
    return std::chrono::duration<double>(Clock::now() - start).count();
}

Application::Application() = default;

Application::Application(GpuDevice* dm, Window* window)
    : m_ExternalGpuDevice(dm)
    , m_ExternalWindow(window)
{
    bindFrameDriver(dm);
}

Application::~Application()
{
    if (!m_shutdownCalled)
        shutdown();
}

bool Application::init(int /*argc*/, const char* const* /*argv*/)
{
    return true;
}

void Application::shutdown()
{
    if (m_shutdownCalled)
        return;

    m_renderThread.stop();

    GpuDevice* dm = device();
    unbindFrameDriver(dm);

    if (m_GpuDevice)
    {
        m_GpuDevice->ReleaseWindowOwnership();
        m_Window.reset();
        m_GpuDevice->Shutdown();
        m_GpuDevice.reset();
    }

    m_shutdownCalled = true;
}

bool Application::initializeGraphics(const GpuDeviceCreateDesc& desc)
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

bool Application::initializeGraphics(int argc, const char* const* argv, GpuDeviceCreateDesc& desc)
{
    InvokePreGpuDeviceInitHook();

    desc.api = ResolveGraphicsAPIFromCommandLine(argc, argv);
    if (desc.headless)
        desc.vsyncEnabled = false;

    return initializeGraphics(desc);
}

GpuDevice* Application::getGpuDevice() const { return device(); }
Window*    Application::getWindow() const    { return window(); }

GpuDevice* Application::device() const
{
    return m_GpuDevice ? m_GpuDevice.get() : m_ExternalGpuDevice;
}

Window* Application::window() const
{
    return m_Window ? m_Window.get() : m_ExternalWindow;
}

void Application::bindFrameDriver(GpuDevice* dm)
{
    if (dm)
        dm->setFrameDriver(this);
}

void Application::unbindFrameDriver(GpuDevice* dm)
{
    if (dm && dm->getFrameDriver() == static_cast<IGpuFrameDriver*>(this))
        dm->setFrameDriver(nullptr);
}

void Application::notifyBackBufferResizing()
{
    onBackBufferResizing();
}

void Application::notifyBackBufferResized(uint32_t width, uint32_t height, uint32_t sampleCount)
{
    onBackBufferResized(width, height, sampleCount);
}

void Application::waitForRenderThreadIdle()
{
    if (m_useDedicatedRenderThread && m_renderThread.isRunning())
        m_renderThread.waitForIdle();
}

void Application::waitForDedicatedRenderThreadIdle()
{
    if (m_useDedicatedRenderThread && m_renderThread.isRunning())
        m_renderThread.waitForIdle();
}

void Application::runGpuWorkOnRenderThread(const std::function<void()>& work)
{
    if (!work)
        return;

    if (m_useDedicatedRenderThread && m_renderThread.isRunning())
        m_renderThread.dispatchAndWait(work);
    else
        work();
}

void Application::requestExit()
{
    m_requestExit = true;
    if (Window* w = window())
        w->setExit(true);
}

void Application::notifyDisplayScaleChanged(float scaleX, float scaleY)
{
    onDisplayScaleChanged(scaleX, scaleY);
}

bool Application::isWindowVisible() const
{
    Window* w = window();
    return w ? w->isVisible() : true;
}

bool Application::isWindowFocused() const
{
    Window* w = window();
    return w ? w->isFocused() : true;
}

void Application::syncDpiScaleFromWindow()
{
    Window* w = window();
    GpuDevice* dm = device();
    if (!w || !dm)
        return;

    dm->m_DPIScaleFactorX = w->getDPIScaleX();
    dm->m_DPIScaleFactorY = w->getDPIScaleY();
}

void Application::syncWindowState()
{
    Window* w = window();
    if (!w)
        return;

    w->onUpdate();
    updateWindowSize();
    syncDpiScaleFromWindow();
}

void Application::updateWindowSize()
{
    GpuDevice* dm = device();
    if (!dm)
        return;

    if (m_useDedicatedRenderThread && m_renderThread.isRunning())
    {
        m_renderThread.dispatchAndWait([dm]() { dm->UpdateWindowSize(); });
    }
    else
    {
        dm->UpdateWindowSize();
    }
}

void Application::animate(double elapsedTime, bool windowIsFocused)
{
    onUpdate(float(elapsedTime), windowIsFocused);
}

void Application::render()
{
    onRender();
}

void Application::finishFrameWithRenderFailure(GpuDevice* gpuDevice, double elapsedTime, double curTime)
{
    gpuDevice->UpdateAverageFrameTime(elapsedTime);
    gpuDevice->m_PreviousFrameTimestamp = curTime;
    ++gpuDevice->m_FrameIndex;
}

bool Application::executeRenderPhase(GpuDevice* gpuDevice, double elapsedTime, double curTime, uint32_t frameIndex)
{
    if (frameIndex == 0 && gpuDevice->m_SkipRenderOnFirstFrame)
        return true;

    if (!gpuDevice->BeginFrame())
        return true;

    uint32_t fi = frameIndex;
    if (gpuDevice->m_SkipRenderOnFirstFrame)
        fi--;
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

bool Application::runFrame(std::optional<double> elapsedTimeOverride)
{
    processEventQueue();

    GpuDevice* gpuDevice = device();
    if (!gpuDevice)
        return false;

    onBeginFrame(*gpuDevice);

    double curTime = GetNow(gpuDevice->m_DeviceParams.headlessDevice);
    double elapsedTime = elapsedTimeOverride.value_or(curTime - gpuDevice->m_PreviousFrameTimestamp);

    const bool windowVisible = isWindowVisible();
    const bool windowFocused = isWindowFocused();

    if (m_useDedicatedRenderThread && !m_renderThread.isRunning())
        m_renderThread.start();

    if (m_useDedicatedRenderThread && m_renderThread.isRunning())
    {
        while (auto completed = m_renderThread.consumeCompletedFrame())
        {
            if (!completed->success)
            {
                finishFrameWithRenderFailure(gpuDevice, completed->elapsedTime, completed->curTime);
                return false;
            }
        }
    }

    if (windowVisible && (windowFocused || shouldRenderWhenUnfocused() || gpuDevice->m_RequestedRenderUnfocused))
    {
        if (gpuDevice->m_PrevDPIScaleFactorX != gpuDevice->m_DPIScaleFactorX ||
            gpuDevice->m_PrevDPIScaleFactorY != gpuDevice->m_DPIScaleFactorY) {
            notifyDisplayScaleChanged(gpuDevice->m_DPIScaleFactorX, gpuDevice->m_DPIScaleFactorY);
            gpuDevice->m_PrevDPIScaleFactorX = gpuDevice->m_DPIScaleFactorX;
            gpuDevice->m_PrevDPIScaleFactorY = gpuDevice->m_DPIScaleFactorY;
        }
        gpuDevice->m_RequestedRenderUnfocused = false;

        if (beforeAnimate) beforeAnimate(*gpuDevice, gpuDevice->m_FrameIndex);
        onBeforeAnimate(*gpuDevice, gpuDevice->m_FrameIndex);
        animate(elapsedTime, true);
#if CAUSTICA_WITH_STREAMLINE
        if (!gpuDevice->m_DeviceParams.headlessDevice) StreamlineIntegration::Get().SimEnd(*gpuDevice);
#endif
        if (afterAnimate) afterAnimate(*gpuDevice, gpuDevice->m_FrameIndex);

        if (!skipRenderPhase())
        {
#if CAUSTICA_WITH_STREAMLINE
        void* slFrameToken = nullptr;
        if (!gpuDevice->m_DeviceParams.headlessDevice)
            slFrameToken = StreamlineIntegration::Get().getFrameTokenForRender();
#endif

        if (m_useDedicatedRenderThread)
        {
            const uint32_t frameIndex = gpuDevice->m_FrameIndex;
            m_renderThread.dispatch([this, gpuDevice, elapsedTime, curTime, frameIndex
#if CAUSTICA_WITH_STREAMLINE
                , slFrameToken
#endif
            ]() {
#if CAUSTICA_WITH_STREAMLINE
                StreamlineIntegration::RenderFrameTokenScope slFrameScope(slFrameToken);
#endif
                const bool ok = executeRenderPhase(gpuDevice, elapsedTime, curTime, frameIndex);
                m_renderThread.notifyFrameCompleted({ok, elapsedTime, curTime});
            });
        }
        else
        {
#if CAUSTICA_WITH_STREAMLINE
            StreamlineIntegration::RenderFrameTokenScope slFrameScope(slFrameToken);
#endif
            if (!executeRenderPhase(gpuDevice, elapsedTime, curTime, gpuDevice->m_FrameIndex))
            {
                finishFrameWithRenderFailure(gpuDevice, elapsedTime, curTime);
                return false;
            }
        }
        }
    }
    else if (windowVisible) {
        if (beforeAnimate) beforeAnimate(*gpuDevice, gpuDevice->m_FrameIndex);
        onBeforeAnimate(*gpuDevice, gpuDevice->m_FrameIndex);
        animate(elapsedTime, false);
        if (afterAnimate) afterAnimate(*gpuDevice, gpuDevice->m_FrameIndex);
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(0));
    gpuDevice->GetDevice()->runGarbageCollection();
    gpuDevice->UpdateAverageFrameTime(elapsedTime);
    gpuDevice->m_PreviousFrameTimestamp = curTime;
    ++gpuDevice->m_FrameIndex;
    return true;
}

void Application::run()
{
    GpuDevice* dm = device();
    if (!dm)
    {
        caustica::error("Application::run requires an initialized GpuDevice");
        return;
    }

    bindFrameDriver(dm);
    dm->m_PreviousFrameTimestamp = GetNow(dm->m_DeviceParams.headlessDevice);

    if (m_useDedicatedRenderThread)
        m_renderThread.start();

#if CAUSTICA_WITH_AFTERMATH
    bool dumpingCrash = false;
#endif

    Window* w = window();
    if (!w)
    {
        if (!dm->m_DeviceParams.headlessDevice)
        {
            caustica::error("Application::run requires a Window");
            return;
        }

        constexpr double kHeadlessFrameTimeSeconds = 1.0 / 60.0;
        while (!m_requestExit)
        {
            processEventQueue();
            if (beforeFrame) beforeFrame(*dm, dm->m_FrameIndex);
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

        bool ok = dm->GetDevice()->waitForIdle();
#if CAUSTICA_WITH_AFTERMATH
        dumpingCrash |= !ok;
        if (dumpingCrash && dm->m_DeviceParams.enableAftermath) AftermathCrashDump::WaitForCrashDump();
#else
        (void)ok;
#endif

        shutdown();
        return;
    }

    installWindowEventCallback();

    while (!w->getExit()) {
        processEventQueue();
#if CAUSTICA_WITH_STREAMLINE
        if (!dm->m_DeviceParams.headlessDevice) StreamlineIntegration::Get().SimStart(*dm);
#endif
        if (beforeFrame) beforeFrame(*dm, dm->m_FrameIndex);
        w->onUpdate();
        updateWindowSize();
        syncDpiScaleFromWindow();
        if (!runFrame()) {
#if CAUSTICA_WITH_AFTERMATH
            dumpingCrash = true;
#endif
            break;
        }
    }

    if (m_useDedicatedRenderThread)
        m_renderThread.waitForIdle();

    bool ok = dm->GetDevice()->waitForIdle();
#if CAUSTICA_WITH_AFTERMATH
    dumpingCrash |= !ok;
    if (dumpingCrash && dm->m_DeviceParams.enableAftermath) AftermathCrashDump::WaitForCrashDump();
#else
    (void)ok;
#endif

    shutdown();
}

bool Application::frame()
{
    return stepFrame();
}

bool Application::stepFrame() { return stepFrame(-1.0); }

bool Application::stepFrame(double dt)
{
    syncWindowState();
    return dt >= 0.0 ? runFrame(std::max(0.0, dt)) : runFrame();
}

} // namespace caustica
