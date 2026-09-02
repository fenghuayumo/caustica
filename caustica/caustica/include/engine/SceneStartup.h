#pragma once

// ENGINE-INTERNAL — not part of the public API (see docs/public-api.md).
// Hosts: EngineApp::create configures startup; do not call registerSceneAppResources yourself.

#include <engine/EngineSceneCallbacks.h>

#include <string>

namespace caustica
{

class App;

// Bootstrap values EngineApp inserts before DefaultPlugins. Startup reads these
// instead of a parallel SceneAppConfig.
struct EngineBootstrap
{
    std::string preferredScene = "default.scene.json";
    bool refreshEnvMapMediaList = true;
    bool applyRenderCli = true;
    bool hasSceneCallbacks = false;
    EngineSceneCallbacks sceneCallbacks{};
};

void initializeSceneApp(App& app);
void registerSceneStartup(App& app);
void registerGpuRenderShutdown(App& app);

} // namespace caustica
