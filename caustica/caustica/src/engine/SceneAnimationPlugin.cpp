#include <engine/ScenePlugins.h>

#include <engine/App.h>
#include <engine/AppSchedules.h>
#include <engine/SceneQuery.h>
#include <engine/RenderFrameApi.h>
#include <engine/SystemLabels.h>
#include <engine/SystemSets.h>

#include <scene/Scene.h>

namespace caustica
{

void refreshEntityWorld(App& app, uint32_t frameIndex)
{
    const std::shared_ptr<Scene> scene = activeScene(app);
    if (!scene)
        return;

    scene->refreshEntityWorldForFrame(frameIndex);
}

void SceneAnimationPlugin::configureSchedules(App& app)
{
    app.addSystem<system_label::SceneAnimate>(
        AppSchedule::update,
        [](SystemContext& ctx) {
            if (!ctx.windowFocused)
                return;

            animate(ctx.app, ctx.deltaTimeSeconds);
        },
        AppSystemOrdering{}.inSet<system_set::Simulation>());

    app.addSystem<system_label::SceneRefreshEntityWorld>(
        AppSchedule::PostUpdate,
        [](SystemContext& ctx) {
            refreshEntityWorld(ctx.app, ctx.frameIndex);
        },
        AppSystemOrdering{}.inSet<system_set::TransformPropagate>());

    app.addSystemAfter<system_label::SceneTickSimulation, system_label::SceneUpdateCamera>(
        AppSchedule::update,
        [](SystemContext& ctx) {
            if (!ctx.windowFocused)
                return;

            tickSimulationAndFrameTiming(ctx.app, ctx.deltaTimeSeconds);
        });
}

} // namespace caustica
