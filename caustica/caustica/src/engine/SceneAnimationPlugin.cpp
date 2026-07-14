#include <engine/SceneSessionPlugins.h>

#include <engine/App.h>
#include <engine/AppSchedules.h>
#include <engine/GpuRenderSubsystem.h>
#include <engine/SceneSessionSystems.h>

#include <scene/Scene.h>
#include <scene/SceneManager.h>

namespace caustica::sceneSession
{

void refreshEntityWorld(App& app, uint32_t frameIndex)
{
    auto* gr = app.tryResource<GpuRenderSubsystem>();
    const std::shared_ptr<Scene> activeScene =
        gr && gr->sceneManager() ? gr->sceneManager()->getScene() : nullptr;
    if (!activeScene)
        return;

    activeScene->refreshEntityWorldForFrame(frameIndex);
}

void registerSceneAnimationPlugin(App& app)
{
    app.addSystem(AppSchedule::update, "SceneSession.animate", [](SystemContext& ctx) {
        if (!ctx.windowFocused)
            return;

        animate(ctx.app, ctx.deltaTimeSeconds);
    });

    app.addSystem(AppSchedule::PostUpdate, "SceneSession.RefreshEntityWorld", [](SystemContext& ctx) {
        refreshEntityWorld(ctx.app, ctx.frameIndex);
    });

    app.addSystemAfter(AppSchedule::update, "SceneSession.TickSimulation", "SceneSession.updateCamera", [](SystemContext& ctx) {
        if (!ctx.windowFocused)
            return;

        tickSimulationAndFrameTiming(ctx.app, ctx.deltaTimeSeconds);
    });
}

} // namespace caustica::sceneSession
