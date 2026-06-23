#include "platform/windows/windows_os.h"
#include "platform/glfw/glfw_window.h"

#include <windows.h>
#include <shellapi.h>
#include <dwmapi.h>
#include <filesystem>

// Forward declaration from the application layer
namespace caustica {
    class Application;
    Application* createApplication();
}

namespace caustica
{

void WindowsOS::init()
{
    // Register GLFW as the default window backend on Windows
    GlfwWindow::makeDefault();
}

void WindowsOS::run()
{
    // Log system information
    MEMORYSTATUSEX status;
    status.dwLength = sizeof(MEMORYSTATUSEX);
    GlobalMemoryStatusEx(&status);

    // Application is created externally (in entry_point.h)
    // and stored via OS::setInstance().
    // The entry point calls:
    //   app->init();
    //   app->run();
    //   app->release();
    // We don't own the Application lifecycle here — this is driven by entry_point.h
}

std::string WindowsOS::getExecutablePath()
{
    WCHAR path[MAX_PATH];
    GetModuleFileNameW(NULL, path, MAX_PATH);

    std::string convertedString = std::filesystem::path(path).string();
    std::replace(convertedString.begin(), convertedString.end(), '\\', '/');
    return convertedString;
}

void WindowsOS::openFileLocation(const std::filesystem::path& path)
{
    ShellExecuteA(NULL, "open",
        std::filesystem::is_directory(path) ? path.string().c_str() : path.parent_path().string().c_str(),
        NULL, NULL, SW_SHOWNORMAL);
}

void WindowsOS::openFileExternal(const std::filesystem::path& path)
{
    ShellExecuteA(NULL, "open", path.string().c_str(), NULL, NULL, SW_SHOWNORMAL);
}

void WindowsOS::openURL(const std::string& url)
{
    ShellExecuteA(NULL, "open", url.c_str(), NULL, NULL, SW_SHOWNORMAL);
}

void WindowsOS::setTitleBarColour(const std::array<float,4>& colour, bool dark)
{
#if WINVER >= 0x0A00
    (void)dark;
    // Note: requires access to the GLFW native window handle,
    // which is available through Application::get().getWindow()->getNativeHandle()
    // This is wired up when Application creates its Window.
    COLORREF captionColor = RGB(
        static_cast<uint8_t>(colour[0] * 255),
        static_cast<uint8_t>(colour[1] * 255),
        static_cast<uint8_t>(colour[2] * 255));
    COLORREF borderColor = 0x201e1e;

    // DWMWA constants for caption/border color (available since Windows 10)
    DwmSetWindowAttribute(nullptr, 34 /*DWMWA_BORDER_COLOR*/, &borderColor, sizeof(borderColor));
    DwmSetWindowAttribute(nullptr, 35 /*DWMWA_CAPTION_COLOR*/, &captionColor, sizeof(captionColor));
#endif
}

} // namespace caustica
