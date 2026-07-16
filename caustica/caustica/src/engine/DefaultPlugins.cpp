#include <engine/DefaultPlugins.h>
#include <engine/App.h>
#include <engine/AssetPlugin.h>
#include <engine/EngineScheduleRegistration.h>
#include <engine/SceneAccess.h>
#include <engine/SceneAppResources.h>
#include <engine/SceneStartup.h>

namespace caustica
{

void DefaultPlugins::build(App& app)
{
    registerAssetPlugin(app);
    registerSceneAppResources(app, appConfig);
    app.emplaceResource<SceneAccess>();
    app.emplaceResource<GpuRenderSubsystem>();
}

void DefaultPlugins::configureSchedules(App& app)
{
    registerEngineScheduleBridge(app);
    registerSceneStartup(app, appConfig);
}

} // namespace caustica
