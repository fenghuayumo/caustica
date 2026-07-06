#include <engine/SceneSessionResources.h>

#include <core/command_line.h>
#include <engine/App.h>
#include <engine/SceneRuntime.h>
#include <engine/SceneRuntimeSubsystem.h>
#include <render/RenderSessionState.h>
#include <render/SessionDiagnostics.h>
#include <render/core/PathTracerSettings.h>
#include <render/RenderRuntimeState.h>

namespace caustica
{

void registerSceneSessionResources(App& app, const SceneRuntimeSubsystemConfig& config)
{
    app.insertResourceRef(config.diagnostics);

    if (config.sessionState)
    {
        app.insertResourceRef(*config.sessionState);
        app.insertResourceRef(config.sessionState->settings);
        app.insertResourceRef(config.sessionState->runtime);
    }
    else
    {
        app.insertResourceRef(config.sceneRuntime.GetPathTracerSettings());
        app.insertResourceRef(config.sceneRuntime.GetRenderRuntimeState());
    }

    if (config.cmdLine)
        app.insertResourceRef(*config.cmdLine);
}

} // namespace caustica
