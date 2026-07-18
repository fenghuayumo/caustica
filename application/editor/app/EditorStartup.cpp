#include "EditorStartup.h"

#include <core/log.h>
#include <core/progress.h>

#include <GLFW/glfw3.h>

#include <algorithm>

namespace caustica::editor
{

namespace
{
    // Keep the OS title bar (min/max/close) on-screen. A client size equal to the
    // monitor resolution pushes decorations off the top edge and looks "fullscreen".
    void ApplyDefaultWindowSizeForLargeDisplays(CommandLineOptions& cmdLine)
    {
        if (cmdLine.fullscreen)
            return;

        if (!glfwInit())
            return;

        GLFWmonitor* primMonitor = glfwGetPrimaryMonitor();
        if (!primMonitor)
            return;

        int workX = 0, workY = 0, workW = 0, workH = 0;
        glfwGetMonitorWorkarea(primMonitor, &workX, &workY, &workW, &workH);
        if (workW <= 0 || workH <= 0)
        {
            if (const GLFWvidmode* mode = glfwGetVideoMode(primMonitor))
            {
                workW = mode->width;
                workH = mode->height;
            }
        }
        if (workW <= 0 || workH <= 0)
            return;

        // Room for title bar + borders + a little breathing space around the window.
        constexpr int kChromeX = 48;
        constexpr int kChromeY = 96;
        const uint32_t maxW = static_cast<uint32_t>(std::max(1280, workW - kChromeX));
        const uint32_t maxH = static_cast<uint32_t>(std::max(720, workH - kChromeY));

        if (workW > 2560 && workH > 1440)
        {
            cmdLine.width = std::min<uint32_t>(2560, maxW);
            cmdLine.height = std::min<uint32_t>(1440, maxH);
        }

        cmdLine.width = std::min(cmdLine.width, maxW);
        cmdLine.height = std::min(cmdLine.height, maxH);
    }

    void ApplyHeadlessConsoleMode(const CommandLineOptions& cmdLine)
    {
        if (cmdLine.nonInteractive)
        {
            caustica::enableOutputToMessageBox(false);
            helpersSetNonInteractive();
        }

        if (cmdLine.noWindow || cmdLine.nonInteractive)
        {
            caustica::consoleApplicationMode();
            caustica::setNativeConsoleVisible(true);
        }
    }
}

bool ProcessEditorStartupCommandLine(int argc, char const* const* argv,
    CommandLineOptions& cmdLine,
    caustica::GpuDeviceCreateDesc& createDesc,
    std::string& preferredScene)
{
    ApplyDefaultWindowSizeForLargeDisplays(cmdLine);

    if (!cmdLine.initFromCommandLine(argc, argv))
        return false;

    if (cmdLine.noWindow)
        cmdLine.nonInteractive = true;

    ApplyHeadlessConsoleMode(cmdLine);

    if (!cmdLine.scene.empty())
        preferredScene = cmdLine.scene;

    if (cmdLine.debug)
        createDesc.enableDebug = true;

    createDesc.backBufferWidth = cmdLine.width;
    createDesc.backBufferHeight = cmdLine.height;
    createDesc.startFullscreen = cmdLine.fullscreen;
    createDesc.adapterIndex = cmdLine.adapterIndex;

    return true;
}

} // namespace caustica::editor
