#pragma once

#include <functional>
#include <memory>

namespace caustica
{

class App;
class EngineApp;

using AppHook = void (*)();

void initializeAppPlatform();
void shutdownAppPlatform();

// Platform bootstrap, startup callback, main loop, and teardown.
//
// startup(app) should register plugins, call initializeGraphics and
// finishStartup, then return true on success.
//
// Returns process exit code (0 = success).
int runApp(App& app, const std::function<bool(App&)>& startup, AppHook preGpuInit = nullptr);

// Run a fully-started EngineApp (requires app().isStarted()), then shut down the platform.
int runEngineApp(std::unique_ptr<EngineApp> engine);

void invokePreGpuDeviceInitHook();

} // namespace caustica
