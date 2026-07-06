#include <engine/DefaultPlugins.h>
#include <engine/App.h>
#include <engine/EngineScheduleRegistration.h>
#include <engine/SceneSessionResources.h>

namespace caustica
{

void DefaultPlugins::build(App& app)
{
    registerSceneSessionResources(app, sessionConfig);
    app.emplaceSubsystem<GpuRenderSubsystem>();
    app.emplaceSubsystem<SceneSessionSubsystem>(sessionConfig);
}

void DefaultPlugins::configureSchedules(App& app)
{
    registerEngineScheduleBridge(app);
}

} // namespace caustica
