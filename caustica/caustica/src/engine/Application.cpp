#include "engine/Application.h"
#include "backend/GpuDevice.h"
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
    unbindFrameDriver(device());
    m_shutdownCalled = true;
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
    if (dm && dm->getFrameDriver() == this)
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
    if (GpuDevice* dm = device())
        dm->UpdateWindowSize();
}

void Application::animate(double elapsedTime, bool windowIsFocused)
{
    onUpdate(float(elapsedTime), windowIsFocused);
}

void Application::render()
{
    onRender();
}

bool Application::runFrame(std::optional<double> elapsedTimeOverride)
{
    processEventQueue();

    GpuDevice* dm = device();
    if (!dm)
        return false;

    auto& DM = *dm;
    double curTime = GetNow(DM.m_DeviceParams.headlessDevice);
    double elapsedTime = elapsedTimeOverride.value_or(curTime - DM.m_PreviousFrameTimestamp);

    const bool windowVisible = isWindowVisible();
    const bool windowFocused = isWindowFocused();

    if (windowVisible && (windowFocused || shouldRenderWhenUnfocused() || DM.m_RequestedRenderUnfocused))
    {
        if (DM.m_PrevDPIScaleFactorX != DM.m_DPIScaleFactorX ||
            DM.m_PrevDPIScaleFactorY != DM.m_DPIScaleFactorY) {
            notifyDisplayScaleChanged(DM.m_DPIScaleFactorX, DM.m_DPIScaleFactorY);
            DM.m_PrevDPIScaleFactorX = DM.m_DPIScaleFactorX;
            DM.m_PrevDPIScaleFactorY = DM.m_DPIScaleFactorY;
        }
        DM.m_RequestedRenderUnfocused = false;

        if (beforeAnimate) beforeAnimate(DM, DM.m_FrameIndex);
        animate(elapsedTime, true);
#if CAUSTICA_WITH_STREAMLINE
        if (!DM.m_DeviceParams.headlessDevice) StreamlineIntegration::Get().SimEnd(DM);
#endif
        if (afterAnimate) afterAnimate(DM, DM.m_FrameIndex);

        if (DM.m_FrameIndex > 0 || !DM.m_SkipRenderOnFirstFrame) {
            if (DM.BeginFrame()) {
                uint32_t fi = DM.m_FrameIndex;
                if (DM.m_SkipRenderOnFirstFrame) fi--;
#if CAUSTICA_WITH_STREAMLINE
                if (!DM.m_DeviceParams.headlessDevice) StreamlineIntegration::Get().RenderStart(DM);
#endif
                if (beforeRender) beforeRender(DM, fi);
                render();
                if (afterRender) afterRender(DM, fi);
#if CAUSTICA_WITH_STREAMLINE
                if (!DM.m_DeviceParams.headlessDevice) {
                    StreamlineIntegration::Get().RenderEnd(DM);
                    StreamlineIntegration::Get().PresentStart(DM);
                }
#endif
                if (beforePresent) beforePresent(DM, fi);
                bool ok = DM.Present();
                if (afterPresent) afterPresent(DM, fi);
#if CAUSTICA_WITH_STREAMLINE
                if (!DM.m_DeviceParams.headlessDevice) StreamlineIntegration::Get().PresentEnd(DM);
#endif
                if (!ok) return false;
            }
        }
    }
    else if (windowVisible) {
        if (beforeAnimate) beforeAnimate(DM, DM.m_FrameIndex);
        animate(elapsedTime, false);
        if (afterAnimate) afterAnimate(DM, DM.m_FrameIndex);
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(0));
    DM.GetDevice()->runGarbageCollection();
    DM.UpdateAverageFrameTime(elapsedTime);
    DM.m_PreviousFrameTimestamp = curTime;
    ++DM.m_FrameIndex;
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
    dm->m_PreviousFrameTimestamp = glfwGetTime();

#if CAUSTICA_WITH_AFTERMATH
    bool dumpingCrash = false;
#endif

    Window* w = window();
    if (!w)
    {
        caustica::error("Application::run requires a Window");
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
