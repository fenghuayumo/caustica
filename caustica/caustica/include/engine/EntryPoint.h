#pragma once

#include <memory>

namespace caustica
{

class EngineApp;

using AppHook = void (*)();

void initializeAppPlatform();
void shutdownAppPlatform();

// Run EngineApp (finishStartup + run), then shut down the platform.
int runEngineApp(std::unique_ptr<EngineApp> engine);

} // namespace caustica
