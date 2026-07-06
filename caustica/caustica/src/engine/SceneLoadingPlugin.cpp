#include <engine/SceneSessionPlugins.h>

#include <engine/App.h>
#include <engine/AppSchedules.h>
#include <engine/SceneSessionSystems.h>

namespace caustica::sceneSession
{

void registerSceneLoadingPlugin(App& app)
{
    app.addSystemAfter(AppSchedule::First, "SceneSession.BeginFrame", "SyncRenderThread", [](AppScheduleContext& ctx) {
        sceneSession::beginFrameScheduled(ctx.app);
    });
}

} // namespace caustica::sceneSession
