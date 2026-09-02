#include "EditorStartup.h"
#include "EditorCommandLine.h"

#include <core/log.h>
#include <core/progress.h>

#include <GLFW/glfw3.h>

#include <algorithm>

namespace caustica::editor
{

namespace
{
    // Keep enough room for the native frame so the title bar and window
    // controls remain visible on the primary monitor.
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
    EngineAppDesc& desc,
    EditorCommandLine& editorCmdLine,
    std::string& preferredScene)
{
    ApplyDefaultWindowSizeForLargeDisplays(desc.cli);
    desc.width = desc.cli.width;
    desc.height = desc.cli.height;

    if (!editorCmdLine.parse(argc, argv))
        return false;

    if (!desc.cli.initFromCommandLine(argc, argv))
        return false;

    if (desc.cli.noWindow)
        desc.cli.nonInteractive = true;

    ApplyHeadlessConsoleMode(desc.cli);

    if (!desc.applyCommandLine(desc.cli))
        return false;

    if (!desc.cli.scene.empty())
        preferredScene = desc.cli.scene;
    desc.scene = preferredScene;
    desc.maximized = !desc.fullscreen;
    return true;
}

} // namespace caustica::editor
