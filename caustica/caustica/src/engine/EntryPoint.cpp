#include "engine/EntryPoint.h"
#include "engine/Application.h"

#include <assets/loader/TextureLoader.h>
#include <assets/AssetSystem.h>
#include <core/JobSystem.h>
#include <platform/engine/os.h>

#include <memory>

#ifdef _WIN32
#include <objbase.h>
#endif

namespace caustica
{

namespace
{
    ApplicationHook s_preGpuInit = nullptr;
}

void InvokePreGpuDeviceInitHook()
{
    if (s_preGpuInit)
        s_preGpuInit();
}

int runApplication(int argc, const char* const* argv,
    ApplicationHook preGpuInit,
    ApplicationHook postInit)
{
    s_preGpuInit = preGpuInit;

#ifdef _WIN32
    const HRESULT comHr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const bool comNeedsUninit = SUCCEEDED(comHr);
#endif

    OS::initialize();
    JobSystem::Initialize();

    std::unique_ptr<Application> app(createApplication());
    if (!app)
    {
        s_preGpuInit = nullptr;
        JobSystem::Shutdown();
#ifdef _WIN32
        if (comNeedsUninit) CoUninitialize();
#endif
        return 1;
    }

    const bool started = app->init(argc, argv);

    s_preGpuInit = nullptr;

    if (postInit)
        postInit();

    if (started)
        app->run();

    app->shutdown();
    AssetSystem::Shutdown();
    JobSystem::Shutdown();

#ifdef _WIN32
    if (comNeedsUninit) CoUninitialize();
#endif
    return started ? 0 : 1;
}

} // namespace caustica
