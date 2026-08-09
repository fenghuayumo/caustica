#include <engine/ScenePlugins.h>

#include <engine/App.h>
#include <engine/AppSchedules.h>
#include <engine/internal/SceneApiInternal.h>
#include <engine/SceneLifecycle.h>
#include <engine/internal/ActiveSceneAccess.h>
#include <engine/SceneQuery.h>
#include <engine/RenderFrameApi.h>
#include <engine/SystemLabels.h>
#include <engine/SystemSets.h>

#include <scene/Scene.h>
#include <scene/SceneManager.h>

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
    // Join CPU import + advance LoadSession (present continues during GpuStreaming).
    app.addSystem<system_label::SceneAnimate>(
        AppSchedule::update,
        [](SystemContext& ctx) {
            if (::SceneManager* manager = detail::sessionManager(ctx.app))
                manager->updateLoading();
            tickLoadSession(ctx.app);

            // Keep the animation clock one-to-one with submitted render frames.
            // During scene streaming/skip-render gaps, windowFocused may remain
            // true solely to pump loading work.
            if (!ctx.runRender)
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
