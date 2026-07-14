#include <engine/SceneSessionPlugins.h>

#include <engine/App.h>
#include <engine/AppSchedules.h>
#include <engine/SceneSessionSystems.h>

namespace caustica::sceneSession
{

void refreshEntityWorld(App& app, uint32_t frameIndex)
{
    const std::shared_ptr<Scene> activeScene = scene(app);
    if (!activeScene)
        return;

    activeScene->refreshEntityWorldForFrame(frameIndex);
}

void registerSceneAnimationPlugin(App& app)
{
    app.addSystem(AppSchedule::update, "SceneSession.animate", [](SystemContext& ctx) {
        if (!ctx.windowFocused)
            return;

        sceneSession::animate(ctx.app, ctx.deltaTimeSeconds);
    });

    app.addSystem(AppSchedule::PostUpdate, "SceneSession.RefreshEntityWorld", [](SystemContext& ctx) {
        sceneSession::refreshEntityWorld(ctx.app, ctx.frameIndex);
    });

    app.addSystemAfter(AppSchedule::update, "SceneSession.TickSimulation", "SceneSession.updateCamera", [](SystemContext& ctx) {
        if (!ctx.windowFocused)
            return;

        sceneSession::tickSimulationAndFrameTiming(ctx.app, ctx.deltaTimeSeconds);
    });
}

} // namespace caustica::sceneSession
