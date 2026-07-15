#include <engine/App.h>
#include <engine/EntryPoint.h>

#include "EditorLaunch.h"
#include "EditorSession.h"

#include <render/passes/debug/Korgi.h>

#include <cstring>

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
void StopSplashBeforeGpuInit()
{
    // Splash screen lifetime is managed in WinMain.
}

SplashScreen* g_activeSplash = nullptr;
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

    auto stopSplash = []() {
        if (g_activeSplash)
        {
            g_activeSplash->stop();
            g_activeSplash = nullptr;
        }
    };

    caustica::editor::EditorSession session;
    caustica::App app;

    const int exitCode = caustica::runApp(
        app,
        [&](caustica::App& targetApp) {
            return caustica::editor::startupEditor(targetApp, session, __argc, (const char**)__argv);
        },
        stopSplash);
#else
    caustica::editor::EditorSession session;
    caustica::App app;

    const int exitCode = caustica::runApp(
        app,
        [&](caustica::App& targetApp) {
            return caustica::editor::startupEditor(targetApp, session, argc, const_cast<const char* const*>(argv));
        });
#endif

    korgi::shutdown();
#ifdef _WIN32
    caustica::shutdownNativeConsole();
#endif
    return exitCode;
}
