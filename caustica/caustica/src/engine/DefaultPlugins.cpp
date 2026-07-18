#include <engine/DefaultPlugins.h>
#include <engine/App.h>
#include <engine/AssetPlugin.h>
#include <engine/GpuRenderSubsystem.h>
#include <engine/GpuSharedCaches.h>
#include <engine/SessionCamera.h>
#include <engine/SceneSession.h>
#include <engine/ScenePlugins.h>
#include <render/worldRenderer/WorldRenderer.h>
#include <engine/ActiveScene.h>
#include <engine/SceneAppResources.h>
#include <engine/SceneStartup.h>

namespace caustica
{

void SceneRuntimePlugin::build(App& app)
{
    registerSceneAppResources(app, appConfig);
    app.emplaceResource<ActiveScene>();
    app.emplaceResource<GpuSharedCaches>();
    app.emplaceResource<SessionCamera>();
    app.emplaceResource<SceneSession>();
    app.emplaceResource<render::WorldRenderer>();
    app.emplaceResource<GpuRenderSubsystem>();
}

void SceneRuntimePlugin::configureSchedules(App& app)
{
    app.registerDefaultSchedules();
    registerSceneStartup(app, appConfig);
}

void DefaultPlugins::build(App& app)
{
    app.addPlugin<AssetPlugin>();
    app.addPlugin<SceneRuntimePlugin>(appConfig);
    app.addPlugin<SceneLoadingPlugin>();
    app.addPlugin<SceneAnimationPlugin>();
    app.addPlugin<CameraPlugin>();
    app.addPlugin<PathTracingPlugin>();
    app.addPlugin<RenderExtractPlugin>();
    app.addPlugin<WindowTitlePlugin>();
}

} // namespace caustica
