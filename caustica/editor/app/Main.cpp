#include "SampleCommon/EditorApplication.h"
#include <cstring>

#ifdef _WIN32
#include <engine/SplashScreen.h>
#endif

static bool WantsHeadlessStartup(int argc, const char* const* argv)
{
    for (int i = 1; i < argc; i++)
    {
        if (std::strcmp(argv[i], "--noWindow") == 0 || std::strcmp(argv[i], "--nonInteractive") == 0)
            return true;
    }
    return false;
}

#ifdef _WIN32
int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int)
#else
int main(int __argc, const char** __argv)
#endif
{
#ifdef _WIN32
    SplashScreen splashScreen;
    if (!WantsHeadlessStartup(__argc, __argv))
        splashScreen.Start(L"loading_splash.png");
#endif

    EditorApplication app;
    const auto status = app.startup(__argc, __argv);

#ifdef _WIN32
    splashScreen.Stop();
#endif

    if (status == EditorApplication::StartupResult::Success)
        app.run();

    return static_cast<int>(status);
}
