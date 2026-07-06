#include <engine/EntryPoint.h>
#include <engine/App.h>

#include <assets/AssetSystem.h>
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
}

void InvokePreGpuDeviceInitHook()
{
    if (s_preGpuInit)
        s_preGpuInit();
}

int runApp(App& app, const std::function<bool(App&)>& startup, AppHook preGpuInit)
{
    s_preGpuInit = preGpuInit;

#ifdef _WIN32
    const HRESULT comHr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const bool comNeedsUninit = SUCCEEDED(comHr);
#endif

    OS::initialize();
    JobSystem::Initialize();

    const auto shutdownOnFailure = [&app]() {
        app.shutdown();
        AssetSystem::shutdown();
        JobSystem::Shutdown();
    };

    if (!startup)
    {
        error("runApp requires a startup callback");
        shutdownOnFailure();
        s_preGpuInit = nullptr;
#ifdef _WIN32
        if (comNeedsUninit)
            CoUninitialize();
#endif
        return 1;
    }

    if (!startup(app))
    {
        shutdownOnFailure();
        s_preGpuInit = nullptr;
#ifdef _WIN32
        if (comNeedsUninit)
            CoUninitialize();
#endif
        return 1;
    }

    if (!app.isStarted())
    {
        error("runApp startup must call finishStartup");
        shutdownOnFailure();
        s_preGpuInit = nullptr;
#ifdef _WIN32
        if (comNeedsUninit)
            CoUninitialize();
#endif
        return 1;
    }

    s_preGpuInit = nullptr;

    app.run();

    AssetSystem::shutdown();
    JobSystem::Shutdown();

#ifdef _WIN32
    if (comNeedsUninit)
        CoUninitialize();
#endif
    return 0;
}

} // namespace caustica
