#include <engine/ScenePlugins.h>

#include <engine/App.h>
#include <engine/AppSchedules.h>
#include <engine/GpuRenderSubsystem.h>
#include <engine/SceneApi.h>

#include <scene/Scene.h>
#include <scene/SceneManager.h>

namespace caustica
{

void refreshEntityWorld(App& app, uint32_t frameIndex)
{
    syncSceneAccess(app);

    auto* gr = app.tryResource<GpuRenderSubsystem>();
    const std::shared_ptr<Scene> activeScene =
        gr && gr->sceneManager() ? gr->sceneManager()->getScene() : nullptr;
    if (!activeScene)
        return;

    activeScene->refreshEntityWorldForFrame(frameIndex);
}

void registerSceneAnimationPlugin(App& app)
{
    app.addSystem(AppSchedule::update, "Scene.animate", [](SystemContext& ctx) {
        if (!ctx.windowFocused)
            return;

        animate(ctx.app, ctx.deltaTimeSeconds);
    });

    app.addSystem(AppSchedule::PostUpdate, "Scene.RefreshEntityWorld", [](SystemContext& ctx) {
        refreshEntityWorld(ctx.app, ctx.frameIndex);
    });

    app.addSystemAfter(AppSchedule::update, "Scene.TickSimulation", "Scene.updateCamera", [](SystemContext& ctx) {
        if (!ctx.windowFocused)
            return;

        tickSimulationAndFrameTiming(ctx.app, ctx.deltaTimeSeconds);
    });
}

} // namespace caustica
