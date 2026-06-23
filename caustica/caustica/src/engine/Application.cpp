#include "engine/Application.h"
#include "backend/GpuDevice.h"
#include "platform/window.h"
#include "platform/Input.h"

#if DONUT_WITH_STREAMLINE
#include <StreamlineIntegration.h>
#endif
#if DONUT_WITH_AFTERMATH
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

void Application::syncWindowState()
{
    if (m_Window) {
        m_Window->onUpdate();
        updateWindowSize();
        m_DM->m_windowVisible  = m_Window->isVisible();
        m_DM->m_windowIsInFocus = m_Window->isFocused();
        m_DM->m_DPIScaleFactorX = m_Window->getDPIScaleX();
        m_DM->m_DPIScaleFactorY = m_Window->getDPIScaleY();
    } else if (m_DM->m_Window) {
        glfwPollEvents();
        updateWindowSize();
    } else {
        m_DM->m_windowVisible  = true;
        m_DM->m_windowIsInFocus = true;
    }
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

    if (DM.m_windowVisible && (DM.m_windowIsInFocus || shouldRenderUnfocused() || DM.m_RequestedRenderUnfocused))
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
#if DONUT_WITH_STREAMLINE
        if (!DM.m_DeviceParams.headlessDevice) StreamlineIntegration::Get().SimEnd(DM);
#endif
        if (afterAnimate) afterAnimate(DM, DM.m_FrameIndex);

        if (DM.m_FrameIndex > 0 || !DM.m_SkipRenderOnFirstFrame) {
            if (DM.BeginFrame()) {
                uint32_t fi = DM.m_FrameIndex;
                if (DM.m_SkipRenderOnFirstFrame) fi--;
#if DONUT_WITH_STREAMLINE
                if (!DM.m_DeviceParams.headlessDevice) StreamlineIntegration::Get().RenderStart(DM);
#endif
                if (beforeRender) beforeRender(DM, fi);
                render();
                if (afterRender) afterRender(DM, fi);
#if DONUT_WITH_STREAMLINE
                if (!DM.m_DeviceParams.headlessDevice) {
                    StreamlineIntegration::Get().RenderEnd(DM);
                    StreamlineIntegration::Get().PresentStart(DM);
                }
#endif
                if (beforePresent) beforePresent(DM, fi);
                bool ok = DM.Present();
                if (afterPresent) afterPresent(DM, fi);
#if DONUT_WITH_STREAMLINE
                if (!DM.m_DeviceParams.headlessDevice) StreamlineIntegration::Get().PresentEnd(DM);
#endif
                if (!ok) return false;
            }
        }
    }
    else if (DM.m_windowVisible) {
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

#if DONUT_WITH_AFTERMATH
    bool dumpingCrash = false;
#endif
    if (m_Window) {
        while (!m_Window->getExit()) {
#if DONUT_WITH_STREAMLINE
            if (!DM.m_DeviceParams.headlessDevice) StreamlineIntegration::Get().SimStart(DM);
#endif
            if (beforeFrame) beforeFrame(DM, DM.m_FrameIndex);
            m_Window->onUpdate(); updateWindowSize();
            DM.m_windowVisible  = m_Window->isVisible();
            DM.m_windowIsInFocus = m_Window->isFocused();
            DM.m_DPIScaleFactorX = m_Window->getDPIScaleX();
            DM.m_DPIScaleFactorY = m_Window->getDPIScaleY();
            if (!runFrame()) break;
        }
    } else {
        GLFWwindow* w = DM.m_Window;
        while (w == nullptr || !glfwWindowShouldClose(w)) {
#if DONUT_WITH_STREAMLINE
            if (!DM.m_DeviceParams.headlessDevice) StreamlineIntegration::Get().SimStart(DM);
#endif
            if (beforeFrame) beforeFrame(DM, DM.m_FrameIndex);
            if (w) { glfwPollEvents(); updateWindowSize(); }
            else   { DM.m_windowVisible = true; DM.m_windowIsInFocus = true; }
            if (!runFrame()) {
#if DONUT_WITH_AFTERMATH
                dumpingCrash = true;
#endif
                break;
            }
        }
    }
    bool ok = DM.GetDevice()->waitForIdle();
#if DONUT_WITH_AFTERMATH
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
