#include <engine/SceneSessionResources.h>



#include <core/command_line.h>

#include <engine/App.h>

#include <engine/SceneSessionSubsystem.h>

#include <engine/SceneViewState.h>

#include <render/RenderSessionState.h>

#include <render/SessionDiagnostics.h>

#include <render/core/PathTracerSettings.h>

#include <render/RenderRuntimeState.h>



namespace caustica

{



void registerSceneSessionResources(App& app, const SceneSessionConfig& config)

{

    app.insertResourceRef(config.viewState);

    app.insertResourceRef(config.diagnostics);



    if (config.sessionState)

    {

        app.insertResourceRef(*config.sessionState);

        app.insertResourceRef(config.sessionState->settings);

        app.insertResourceRef(config.sessionState->runtime);

    }



    if (config.cmdLine)

        app.insertResourceRef(*config.cmdLine);

}



} // namespace caustica

