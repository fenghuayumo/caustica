#include <engine/SceneAppResources.h>
#include <engine/SceneStartup.h>

#include <core/command_line.h>
#include <engine/App.h>
#include <engine/SceneViewState.h>
#include <render/RenderAppState.h>
#include <render/AppDiagnostics.h>
#include <render/core/PathTracerSettings.h>
#include <render/RenderRuntimeState.h>

namespace caustica
{

void registerSceneAppResources(App& app, const SceneAppConfig& config)
{
    app.insertResourceRef(config.viewState);
    app.insertResourceRef(config.diagnostics);

    if (config.renderState)
    {
        app.insertResourceRef(*config.renderState);
        app.insertResourceRef(config.renderState->settings);
        app.insertResourceRef(config.renderState->runtime);
    }

    if (config.cmdLine)
        app.insertResourceRef(*config.cmdLine);
}

} // namespace caustica
