#include "engine/Application.h"
#include "backend/GpuDevice.h"
#include "platform/window.h"
#include "platform/Input.h"

#if CAUSTICA_WITH_STREAMLINE
#include <StreamlineIntegration.h>
#endif
#if CAUSTICA_WITH_AFTERMATH
#include "AftermathCrashDump.h"
#endif

#include <chrono>
#include <thread>

namespace caustica {

static double GetNow(bool headless)
{
    if (!headless) return glfwGetTime();
    using Clock = std::chrono::steady_clock;
    static const auto start = Clock::now();
    return std::chrono::duration<double>(Clock::now() - start).count();
}

Application::Application(GpuDevice* dm, Window* window)
    : m_DM(dm), m_Window(window) {}

bool Application::isWindowVisible() const
{
    return m_Window ? m_Window->isVisible() : true;
}

bool Application::isWindowFocused() const
{
    return m_Window ? m_Window->isFocused() : true;
}

void Application::syncDpiScaleFromWindow()
{
    if (!m_Window)
        return;

    m_DM->m_DPIScaleFactorX = m_Window->getDPIScaleX();
    m_DM->m_DPIScaleFactorY = m_Window->getDPIScaleY();
}

void Application::syncWindowState()
{
    if (!m_Window)
        return;

    m_Window->onUpdate();
    updateWindowSize();
    syncDpiScaleFromWindow();
}

void Application::updateWindowSize()  { m_DM->UpdateWindowSize(); }

bool Application::shouldRenderUnfocused() const
{
    for (auto* pass : m_DM->m_vRenderPasses)
        if (pass->ShouldRenderUnfocused()) return true;
    return false;
}

void Application::animate(double elapsedTime, bool windowIsFocused)
{
    for (auto* pass : m_DM->m_vRenderPasses) {
        if (windowIsFocused || pass->ShouldAnimateUnfocused()) {
            pass->Animate(float(elapsedTime));
            pass->SetLatewarpOptions();
        }
    }
}

void Application::render()
{
    for (auto* pass : m_DM->m_vRenderPasses) {
        nvrhi::IFramebuffer* fb = m_DM->GetCurrentFramebuffer(pass->SupportsDepthBuffer());
        pass->Render(fb);
    }
}

bool Application::runFrame(std::optional<double> elapsedTimeOverride)
{
    auto& DM = *m_DM;  // shorthand for friend access
    double curTime = GetNow(DM.m_DeviceParams.headlessDevice);
    double elapsedTime = elapsedTimeOverride.value_or(curTime - DM.m_PreviousFrameTimestamp);

    if (!DM.m_DeviceParams.headlessDevice)
        if (DM.m_Input) DM.m_Input->pollJoysticks();

    const bool windowVisible = isWindowVisible();
    const bool windowFocused = isWindowFocused();

    if (windowVisible && (windowFocused || shouldRenderUnfocused() || DM.m_RequestedRenderUnfocused))
    {
        if (DM.m_PrevDPIScaleFactorX != DM.m_DPIScaleFactorX ||
            DM.m_PrevDPIScaleFactorY != DM.m_DPIScaleFactorY) {
            DM.DisplayScaleChanged();
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
    auto& DM = *m_DM;
    DM.m_PreviousFrameTimestamp = glfwGetTime();

#if CAUSTICA_WITH_AFTERMATH
    bool dumpingCrash = false;
#endif
    if (!m_Window)
    {
        caustica::error("Application::run requires a Window");
        return;
    }

    while (!m_Window->getExit()) {
#if CAUSTICA_WITH_STREAMLINE
        if (!DM.m_DeviceParams.headlessDevice) StreamlineIntegration::Get().SimStart(DM);
#endif
        if (beforeFrame) beforeFrame(DM, DM.m_FrameIndex);
        m_Window->onUpdate();
        updateWindowSize();
        syncDpiScaleFromWindow();
        if (!runFrame()) {
#if CAUSTICA_WITH_AFTERMATH
            dumpingCrash = true;
#endif
            break;
        }
    }

    bool ok = DM.GetDevice()->waitForIdle();
#if CAUSTICA_WITH_AFTERMATH
    dumpingCrash |= !ok;
    if (dumpingCrash && DM.m_DeviceParams.enableAftermath) AftermathCrashDump::WaitForCrashDump();
#else
    (void)ok;
#endif
}

bool Application::stepFrame() { return stepFrame(-1.0); }

bool Application::stepFrame(double dt)
{
    syncWindowState();
    return dt >= 0.0 ? runFrame(std::max(0.0, dt)) : runFrame();
}

} // namespace caustica
