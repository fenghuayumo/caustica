#include <engine/DefaultPlugins.h>
#include <engine/App.h>
#include <engine/EngineScheduleRegistration.h>
#include <engine/SceneSessionResources.h>
#include <engine/SceneSessionStartup.h>

namespace caustica
{

void DefaultPlugins::build(App& app)
{
    registerSceneSessionResources(app, sessionConfig);
    app.emplaceResource<GpuRenderSubsystem>();
}

void DefaultPlugins::configureSchedules(App& app)
{
    registerEngineScheduleBridge(app);
    registerSceneSessionStartup(app, sessionConfig);
}

} // namespace caustica
