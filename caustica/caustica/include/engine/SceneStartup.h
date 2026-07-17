#pragma once

#include <core/command_line.h>

#include <engine/EngineSceneCallbacks.h>
#include <engine/SceneViewState.h>

#include <render/RenderAppState.h>
#include <render/AppDiagnostics.h>

#include <string>

namespace caustica
{

class App;

struct SceneAppConfig
{
    SceneViewState& viewState;
    render::AppDiagnostics& diagnostics;
    std::string preferredScene;

    render::RenderAppState* renderState = nullptr;
    const CommandLineOptions* cmdLine = nullptr;
    bool refreshEnvMapMediaList = true;
    bool applyCmdLineToRenderState = true;
    bool hasSceneCallbacks = false;
    EngineSceneCallbacks sceneCallbacks{};
};

void initializeSceneApp(App& app, const SceneAppConfig& config);

void registerSceneStartup(App& app, const SceneAppConfig& config);
void registerGpuRenderShutdown(App& app);

} // namespace caustica
