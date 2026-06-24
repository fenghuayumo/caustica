#include "AdvancedSample.h"
#include <SampleCommon/SampleBaseApp.h>
#include <cstring>

#ifdef _WIN32
#include <engine/SplashScreen.h>
#endif

class AdvancedSample : public SampleBaseApp
{
    std::unique_ptr<Sample> CreateMainRenderPass(caustica::GpuDevice& deviceManager, const CommandLineOptions& cmdLineOptions) override
    {
        return std::make_unique<AdvancedPathTracer>(deviceManager, cmdLineOptions);
    }
};

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
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow)
#else
int main(int __argc, const char** __argv)
#endif
{
#ifdef _WIN32
    SplashScreen splashScreen;
    if (!WantsHeadlessStartup(__argc, __argv))
        splashScreen.Start(L"loading_splash.png");
#endif

    AdvancedSample example;

    // Run the sample app
    const auto status = example.Init(__argc, __argv);
    
#ifdef _WIN32
    splashScreen.Stop();
#endif

    if (status == SampleBaseApp::InitReturnCodes::Success)
    {
        example.RunMainLoop();

        example.End();
    }
    
    return static_cast<int>(status);
}
