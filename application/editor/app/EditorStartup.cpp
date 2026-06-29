#include "EditorStartup.h"

#include <core/log.h>
#include <core/progress.h>

#include <GLFW/glfw3.h>

namespace caustica::editor
{

namespace
{
    void ApplyDefaultWindowSizeForLargeDisplays(CommandLineOptions& cmdLine)
    {
        if (!glfwInit())
            return;

        if (GLFWmonitor* primMonitor = glfwGetPrimaryMonitor())
        {
            if (const GLFWvidmode* mode = glfwGetVideoMode(primMonitor))
            {
                if (mode->width > 2560 && mode->height > 1440)
                {
                    cmdLine.width = 2560;
                    cmdLine.height = 1440;
                }
            }
        }
    }

    void ApplyHeadlessConsoleMode(const CommandLineOptions& cmdLine)
    {
        if (cmdLine.nonInteractive)
        {
            caustica::EnableOutputToMessageBox(false);
            HelpersSetNonInteractive();
        }

        if (cmdLine.noWindow || cmdLine.nonInteractive)
            caustica::ConsoleApplicationMode();
    }
}

bool ProcessEditorStartupCommandLine(int argc, char const* const* argv,
    CommandLineOptions& cmdLine,
    caustica::GpuDeviceCreateDesc& createDesc,
    std::string& preferredScene)
{
    ApplyDefaultWindowSizeForLargeDisplays(cmdLine);

    if (!cmdLine.InitFromCommandLine(argc, argv))
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
