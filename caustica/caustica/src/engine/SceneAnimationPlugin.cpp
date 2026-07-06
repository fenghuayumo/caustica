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
    app.addSystem(AppSchedule::Update, "SceneSession.Animate", [](AppScheduleContext& ctx) {
        if (!ctx.windowFocused)
            return;

        sceneSession::animate(ctx.app, ctx.deltaTimeSeconds);
    });

    app.addSystem(AppSchedule::PostUpdate, "SceneSession.RefreshEntityWorld", [](AppScheduleContext& ctx) {
        sceneSession::refreshEntityWorld(ctx.app, ctx.frameIndex);
    });
}

} // namespace caustica::sceneSession
