#include <engine/EntryPoint.h>
#include "EditorApplication.h"

#include <core/JobSystem.h>
#include <platform/engine/os.h>

#include <cstring>

#ifdef _WIN32
#include <engine/SplashScreen.h>
#endif

namespace caustica
{

Application* createApplication()
{
    return new caustica::editor::EditorApplication();
}

} // namespace caustica

namespace
{

bool WantsHeadlessStartup(int argc, const char* const* argv)
{
    for (int i = 1; i < argc; i++)
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
        g_activeSplash->Stop();
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
    caustica::OS::initialize();
    caustica::JobSystem::Initialize();

#ifdef _WIN32
    SplashScreen splashScreen;
    if (!WantsHeadlessStartup(__argc, (const char**)__argv))
    {
        splashScreen.Start(L"loading_splash.png");
        g_activeSplash = &splashScreen;
    }

    const int exitCode = caustica::runApplication(__argc, (const char**)__argv, StopSplashBeforeGpuInit);
#else
    const int exitCode = caustica::runApplication(argc, const_cast<const char* const*>(argv));
#endif

    return exitCode;
}
