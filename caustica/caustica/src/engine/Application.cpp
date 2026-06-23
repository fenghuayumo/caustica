#include "engine/Application.h"
#include "engine/DeviceManager.h"
#include "platform/window.h"

#if DONUT_WITH_STREAMLINE
#include <StreamlineIntegration.h>
#endif
#if DONUT_WITH_AFTERMATH
#include "AftermathCrashDump.h"
#endif

namespace caustica {

Application::Application(DeviceManager* dm, Window* window)
    : m_DM(dm)
    , m_Window(window)
{
}

void Application::syncWindowState()
{
    if (m_Window)
    {
        m_Window->onUpdate();  // polls GLFW events, dispatches callbacks
        m_DM->UpdateWindowSize();
        m_DM->m_windowVisible  = m_Window->isVisible();
        m_DM->m_windowIsInFocus = m_Window->isFocused();
        m_DM->m_DPIScaleFactorX = m_Window->getDPIScaleX();
        m_DM->m_DPIScaleFactorY = m_Window->getDPIScaleY();
    }
    else if (m_DM->m_Window)
    {
        glfwPollEvents();
        if (m_DM->m_DeviceParams.headlessDevice)
        {
            m_DM->m_windowVisible  = true;
            m_DM->m_windowIsInFocus = true;
        }
        else
        {
            m_DM->UpdateWindowSize();
        }
    }
    else
    {
        m_DM->m_windowVisible  = true;
        m_DM->m_windowIsInFocus = true;
    }
}

void Application::run()
{
    m_DM->m_PreviousFrameTimestamp = glfwGetTime();

#if DONUT_WITH_AFTERMATH
    bool dumpingCrash = false;
#endif
    if (m_Window)
    {
        while (!m_Window->getExit())
        {
#if DONUT_WITH_STREAMLINE
            if (!m_DM->m_DeviceParams.headlessDevice)
                StreamlineIntegration::Get().SimStart(*m_DM);
#endif
            if (beforeFrame) beforeFrame(*m_DM, m_DM->m_FrameIndex);

            m_Window->onUpdate();
            m_DM->UpdateWindowSize();
            m_DM->m_windowVisible  = m_Window->isVisible();
            m_DM->m_windowIsInFocus = m_Window->isFocused();
            m_DM->m_DPIScaleFactorX = m_Window->getDPIScaleX();
            m_DM->m_DPIScaleFactorY = m_Window->getDPIScaleY();

            if (!m_DM->AnimateRenderPresent()) break;
        }
    }
    else
    {
        GLFWwindow* glfwWin = m_DM->m_Window;
        while (glfwWin == nullptr || !glfwWindowShouldClose(glfwWin))
        {
#if DONUT_WITH_STREAMLINE
            if (!m_DM->m_DeviceParams.headlessDevice)
                StreamlineIntegration::Get().SimStart(*m_DM);
#endif
            if (beforeFrame) beforeFrame(*m_DM, m_DM->m_FrameIndex);
            if (glfwWin != nullptr)
            {
                glfwPollEvents();
                m_DM->UpdateWindowSize();
            }
            else
            {
                m_DM->m_windowVisible  = true;
                m_DM->m_windowIsInFocus = true;
            }
            if (!m_DM->AnimateRenderPresent())
            {
#if DONUT_WITH_AFTERMATH
                dumpingCrash = true;
#endif
                break;
            }
        }
    }

    bool waitSuccess = m_DM->GetDevice()->waitForIdle();
#if DONUT_WITH_AFTERMATH
    dumpingCrash |= !waitSuccess;
    if (dumpingCrash && m_DM->m_DeviceParams.enableAftermath)
        AftermathCrashDump::WaitForCrashDump();
#else
    (void)waitSuccess;
#endif
}

bool Application::stepFrame()
{
    return stepFrame(-1.0);
}

bool Application::stepFrame(double fixedElapsedTimeSeconds)
{
    syncWindowState();

    if (fixedElapsedTimeSeconds >= 0.0)
        return m_DM->AnimateRenderPresent(std::max(0.0, fixedElapsedTimeSeconds));
    return m_DM->AnimateRenderPresent();
}

} // namespace caustica
