#pragma once

#include <functional>

namespace caustica
{

class App;

using AppHook = void (*)();

// Platform bootstrap, startup callback, main loop, and teardown.
//
// startup(app) should register plugins, call initializeGraphics and
// initializeEngine, then return true on success.
//
// Returns process exit code (0 = success).
int runApp(App& app, const std::function<bool(App&)>& startup, AppHook preGpuInit = nullptr);

void InvokePreGpuDeviceInitHook();

} // namespace caustica
