#include <engine/DefaultPlugins.h>
#include <engine/App.h>
#include <engine/EngineScheduleRegistration.h>
#include <engine/SceneSessionResources.h>

namespace caustica
{

void DefaultPlugins::build(App& app)
{
    registerSceneSessionResources(app, sceneConfig);
    app.emplaceSubsystem<GpuRenderSubsystem>();
    app.emplaceSubsystem<SceneRuntimeSubsystem>(sceneConfig);
}

void DefaultPlugins::configureSchedules(App& app)
{
    registerEngineScheduleBridge(app);
}

} // namespace caustica
