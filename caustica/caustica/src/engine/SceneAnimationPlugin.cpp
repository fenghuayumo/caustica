#include <engine/ScenePlugins.h>

#include <engine/App.h>
#include <engine/AppSchedules.h>
#include <engine/SceneQuery.h>
#include <engine/RenderFrameApi.h>

#include <scene/Scene.h>

namespace caustica
{

void refreshEntityWorld(App& app, uint32_t frameIndex)
{
    syncSceneAccess(app);

    const std::shared_ptr<Scene> scene = activeScene(app);
    if (!scene)
        return;

    scene->refreshEntityWorldForFrame(frameIndex);
}

void SceneAnimationPlugin::configureSchedules(App& app)
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
