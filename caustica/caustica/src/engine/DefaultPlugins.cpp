#include <engine/internal/DefaultPlugins.h>
#include <engine/App.h>
#include <engine/AssetPlugin.h>
#include <engine/Input.h>
#include <engine/internal/GpuRenderSubsystem.h>
#include <engine/GpuSharedCaches.h>
#include <render/core/CameraController.h>
#include <engine/ResolvedActiveCamera.h>
#include <engine/SensorApi.h>
#include <engine/SceneSession.h>
#include <engine/ScenePlugins.h>
#include <render/WorldRenderer.h>
#include <engine/ActiveScene.h>
#include <engine/Time.h>
#include <engine/SceneAppResources.h>
#include <engine/SceneStartup.h>
#include <engine/SystemLabels.h>

namespace caustica
{

void InputPlugin::build(App& app)
{
    app.emplaceResource<InputState>();
    app.emplaceResource<CameraInputConfig>();
    app.emplaceResource<CameraInputGate>();
}

void InputPlugin::configureSchedules(App& app)
{
    (void)app;
}

void SceneRuntimePlugin::build(App& app)
{
    registerSceneAppResources(app);
    app.emplaceResource<ActiveScene>();
    app.emplaceResource<Time>();
    app.emplaceResource<GpuSharedCaches>();
    app.emplaceResource<CameraController>();
    app.emplaceResource<ResolvedActiveCamera>();
    app.emplaceResource<RenderProductRegistry>();
    app.emplaceResource<SceneSession>();
    app.emplaceResource<render::WorldRenderer>();
    app.emplaceResource<GpuRenderSubsystem>();
}

void SceneRuntimePlugin::configureSchedules(App& app)
{
    app.registerDefaultSchedules();
    registerSceneStartup(app);
}

void DefaultPlugins::build(App& app)
{
    app.addPlugin<InputPlugin>();
    app.addPlugin<AssetPlugin>();
    app.addPlugin<SceneRuntimePlugin>();
    app.addPlugin<SceneLoadingPlugin>();
    app.addPlugin<SceneAnimationPlugin>();
    app.addPlugin<CameraPlugin>();
    app.addPlugin<PathTracingPlugin>();
    app.addPlugin<RenderExtractPlugin>();
    app.addPlugin<WindowTitlePlugin>();
}

} // namespace caustica
