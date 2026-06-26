#pragma once

// =============================================================================
// EntryPoint — Platform entry helpers (DIVSHOT-style).
//
// Application executables implement createApplication() and call runApplication()
// from their platform entry (WinMain / main).  Splash screens and other
// app-specific pre/post hooks stay in the application layer.
// =============================================================================

namespace caustica
{

class Application;

Application* createApplication();

using ApplicationHook = void (*)();

int runApplication(int argc, const char* const* argv,
    ApplicationHook preGpuInit = nullptr,
    ApplicationHook postInit = nullptr);

void InvokePreGpuDeviceInitHook();

} // namespace caustica
