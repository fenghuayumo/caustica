// =============================================================================
// caustica Entry Point
// =============================================================================
// This header provides the platform-independent main()/WinMain() entry point
// for caustica applications. The client application (e.g. editor/) defines:
//
//   namespace caustica {
//       Application* createApplication();
//   }
//
// The entry point follows the 4-layer architecture:
//   Layer 1 (Platform):  OS, Window, FileSystem, Timer
//   Layer 2 (Core):      CoreSystem, JobSystem, Memory
//   Layer 3 (Engine):    Application (created here)
//   Layer 4 (Render):    (initialized by Application)
// =============================================================================

#pragma once

// Include the new platform and core layers
#include "core/core_system.h"

#if defined(_WIN32)
    #include "platform/windows/windows_os.h"

    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <windows.h>
    #include <shellapi.h>

#elif defined(__linux__)
    #include "platform/unix/unix_os.h"

#elif defined(__APPLE__)
    #include "platform/macos/mac_os.h"
#endif

// The client application must implement this function
namespace caustica {
    class Application;
    extern Application* createApplication();
}

// =============================================================================
// Windows Entry Point
// =============================================================================
#if defined(_WIN32)

#ifdef CAUSTICA_PRODUCTION_BUILD
    // Production: WinMain (no console)
    int APIENTRY WinMain(HINSTANCE hInst, HINSTANCE hInstPrev, PSTR cmdline, int cmdshow)
    {
        // Convert WinMain arguments to argc/argv
        int argc = __argc;
        char** argv = __argv;
        (void)hInst; (void)hInstPrev; (void)cmdline; (void)cmdshow;

        if (!caustica::coresystem::init(argc, argv))
            return 1;

        auto* windowsOS = new caustica::WindowsOS();
        caustica::OS::setInstance(windowsOS);
        windowsOS->init();

        // Create the application (defined by client)
        auto* app = caustica::createApplication();
        app->init();
        app->run();
        app->release();
        delete app;

        delete windowsOS;
        caustica::coresystem::shutdown();
        return 0;
    }
#else
    // Debug/Development: console main()
    #pragma comment(linker, "/subsystem:console")

    int main(int argc, char** argv)
    {
        if (!caustica::coresystem::init(argc, argv))
            return 1;

        auto* windowsOS = new caustica::WindowsOS();
        caustica::OS::setInstance(windowsOS);
        windowsOS->init();

        // Create the application (defined by client)
        auto* app = caustica::createApplication();
        app->init();
        app->run();
        app->release();
        delete app;

        delete windowsOS;
        caustica::coresystem::shutdown();
        return 0;
    }
#endif // CAUSTICA_PRODUCTION_BUILD

// =============================================================================
// Linux Entry Point
// =============================================================================
#elif defined(__linux__)

int main(int argc, char** argv)
{
    if (!caustica::coresystem::init(argc, argv))
        return 1;

    auto* unixOS = new caustica::UnixOS();
    caustica::OS::setInstance(unixOS);
    unixOS->init();

    auto* app = caustica::createApplication();
    app->init();
    app->run();
    app->release();
    delete app;

    delete unixOS;
    caustica::coresystem::shutdown();
    return 0;
}

// =============================================================================
// macOS Entry Point
// =============================================================================
#elif defined(__APPLE__)

int main(int argc, char** argv)
{
    if (!caustica::coresystem::init(argc, argv))
        return 1;

    auto* macosOS = new caustica::MacOS();
    caustica::OS::setInstance(macosOS);
    macosOS->init();

    auto* app = caustica::createApplication();
    app->init();
    app->run();
    app->release();
    delete app;

    delete macosOS;
    caustica::coresystem::shutdown();
    return 0;
}

#endif
