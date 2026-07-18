#include <engine/EngineApp.h>
#include <engine/EntryPoint.h>

#include "EditorLaunch.h"
#include "EditorHost.h"

#include <render/passes/debug/Korgi.h>

#include <cstring>
#include <memory>

#ifdef _WIN32
#include <engine/SplashScreen.h>
#include <core/log.h>
#endif

namespace
{

bool WantsHeadlessStartup(int argc, const char* const* argv)
{
    for (int i = 1; i < argc; ++i)
    {
        if (std::strcmp(argv[i], "--noWindow") == 0 || std::strcmp(argv[i], "--nonInteractive") == 0)
            return true;
    }
    return false;
}

#ifdef _WIN32
SplashScreen* g_activeSplash = nullptr;

void StopSplashBeforeGpuInit()
{
    if (g_activeSplash)
    {
        g_activeSplash->stop();
        g_activeSplash = nullptr;
    }
}
#endif

} // namespace

#ifdef _WIN32
int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int)
#else
int main(int argc, char** argv)
#endif
{
#ifdef _WIN32
    // Allocate a console for engine logs. Release starts hidden; Debug starts visible.
    // F1 toggles visibility. Headless/--noWindow forces visible in EditorStartup.
#if defined(_DEBUG)
    caustica::initNativeConsole(/*visibleByDefault=*/true);
#else
    caustica::initNativeConsole(/*visibleByDefault=*/false);
#endif

    if (WantsHeadlessStartup(__argc, (const char**)__argv))
        caustica::setNativeConsoleVisible(true);

    SplashScreen splashScreen;
    if (!WantsHeadlessStartup(__argc, (const char**)__argv))
    {
        splashScreen.start(L"loading_splash.png");
        g_activeSplash = &splashScreen;
    }
#endif

    caustica::initializeAppPlatform();

    caustica::editor::EditorHost host;

#ifdef _WIN32
    auto engine = caustica::editor::createEditorEngine(
        host, __argc, (const char**)__argv, StopSplashBeforeGpuInit);
#else
    auto engine = caustica::editor::createEditorEngine(
        host, argc, const_cast<const char* const*>(argv));
#endif

    if (!engine)
    {
        caustica::shutdownAppPlatform();
        korgi::shutdown();
#ifdef _WIN32
        caustica::shutdownNativeConsole();
#endif
        return 1;
    }

    const int exitCode = caustica::runEngineApp(std::move(engine));

    korgi::shutdown();
#ifdef _WIN32
    caustica::shutdownNativeConsole();
#endif
    return exitCode;
}
