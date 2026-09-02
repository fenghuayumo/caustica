#include <engine/EntryPoint.h>
#include <engine/EngineApp.h>

#include <core/task/TaskRuntime.h>
#include <core/log.h>
#include <platform/engine/os.h>

#ifdef _WIN32
#include <objbase.h>
#endif

namespace caustica
{

#ifdef _WIN32
namespace
{
bool s_comNeedsUninit = false;
}
#endif

void initializeAppPlatform()
{
#ifdef _WIN32
    const HRESULT comHr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    s_comNeedsUninit = SUCCEEDED(comHr);
#endif

    OS::initialize();
    task::initialize();
}

void shutdownAppPlatform()
{
    task::shutdown();

#ifdef _WIN32
    if (s_comNeedsUninit)
    {
        CoUninitialize();
        s_comNeedsUninit = false;
    }
#endif
}

int runEngineApp(std::unique_ptr<EngineApp> engine)
{
    if (!engine || !engine->isValid())
    {
        error("runEngineApp requires a valid EngineApp");
        if (engine)
            engine->shutdown();
        shutdownAppPlatform();
        return 1;
    }

    if (!engine->finishStartup())
    {
        error("runEngineApp: finishStartup failed");
        engine->shutdown();
        shutdownAppPlatform();
        return 1;
    }

    engine->run();
    shutdownAppPlatform();
    return 0;
}

} // namespace caustica
