#pragma once

#include <core/command_line.h>

#include <engine/GpuRenderSubsystem.h>
#include <engine/SceneViewState.h>

#include <render/RenderSessionState.h>
#include <render/SessionDiagnostics.h>

#include <string>

namespace caustica
{

class App;

struct SceneSessionConfig
{
    SceneViewState& viewState;
    render::SessionDiagnostics& diagnostics;
    std::string preferredScene;

    render::RenderSessionState* sessionState = nullptr;
    const CommandLineOptions* cmdLine = nullptr;
    bool refreshEnvMapMediaList = true;
    bool applyCmdLineToSessionState = true;
    bool hasSceneCallbacks = false;
    EngineSceneCallbacks sceneCallbacks{};
};

void initializeSceneSession(App& app, const SceneSessionConfig& config);

void registerSceneSessionStartup(App& app, const SceneSessionConfig& config);
void registerGpuRenderShutdown(App& app);

} // namespace caustica
