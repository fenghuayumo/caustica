#include <engine/EntryPoint.h>
#include <engine/App.h>
#include <engine/EngineApp.h>

#include <assets/loader/TextureLoader.h>
#include <core/JobSystem.h>
#include <core/log.h>
#include <platform/engine/os.h>

#ifdef _WIN32
#include <objbase.h>
#endif

namespace caustica
{

namespace
{
AppHook s_preGpuInit = nullptr;

#ifdef _WIN32
bool s_comNeedsUninit = false;
#endif
}

void invokePreGpuDeviceInitHook()
{
    if (s_preGpuInit)
        s_preGpuInit();
}

void initializeAppPlatform()
{
#ifdef _WIN32
    const HRESULT comHr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    s_comNeedsUninit = SUCCEEDED(comHr);
#endif

    OS::initialize();
    JobSystem::Initialize();
}

void shutdownAppPlatform()
{
    JobSystem::shutdown();

#ifdef _WIN32
    if (s_comNeedsUninit)
    {
        CoUninitialize();
        s_comNeedsUninit = false;
    }
#endif
}

int runApp(App& app, const std::function<bool(App&)>& startup, AppHook preGpuInit)
{
    s_preGpuInit = preGpuInit;

    initializeAppPlatform();

    const auto shutdownOnFailure = [&app]() {
        app.shutdown();
        shutdownAppPlatform();
    };

    if (!startup)
    {
        error("runApp requires a startup callback");
        shutdownOnFailure();
        s_preGpuInit = nullptr;
        return 1;
    }

    if (!startup(app))
    {
        shutdownOnFailure();
        s_preGpuInit = nullptr;
        return 1;
    }

    if (!app.isStarted())
    {
        error("runApp startup must call finishStartup");
        shutdownOnFailure();
        s_preGpuInit = nullptr;
        return 1;
    }

    s_preGpuInit = nullptr;

    app.run();

    shutdownAppPlatform();
    return 0;
}

int runEngineApp(std::unique_ptr<EngineApp> engine)
{
    if (!engine || !engine->isValid() || !engine->app().isStarted())
    {
        error("runEngineApp requires a started EngineApp");
        if (engine)
            engine->shutdown();
        shutdownAppPlatform();
        return 1;
    }

    engine->run();
    shutdownAppPlatform();
    return 0;
}

} // namespace caustica
