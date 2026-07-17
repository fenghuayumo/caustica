#include <engine/DefaultPlugins.h>
#include <engine/App.h>
#include <engine/AssetPlugin.h>
#include <engine/GpuRenderSubsystem.h>
#include <engine/GpuSharedCaches.h>
#include <engine/SessionCamera.h>
#include <engine/SceneSession.h>
#include <render/worldRenderer/WorldRenderer.h>
#include <engine/SceneAccess.h>
#include <engine/SceneAppResources.h>
#include <engine/SceneStartup.h>

namespace caustica
{

void DefaultPlugins::build(App& app)
{
    AssetPlugin{}.build(app);
    registerSceneAppResources(app, appConfig);
    app.emplaceResource<SceneAccess>();
    app.emplaceResource<GpuSharedCaches>();
    app.emplaceResource<SessionCamera>();
    app.emplaceResource<SceneSession>();
    app.emplaceResource<render::WorldRenderer>();
    app.emplaceResource<GpuRenderSubsystem>();
}

void DefaultPlugins::configureSchedules(App& app)
{
    AssetPlugin{}.configureSchedules(app);
    app.registerDefaultSchedules();
    registerSceneStartup(app, appConfig);
}

} // namespace caustica
