#include <engine/SceneSessionPlugins.h>

#include <engine/App.h>
#include <engine/AppSchedules.h>
#include <engine/SceneSessionSystems.h>

namespace caustica::sceneSession
{

void registerPathTracingPlugin(App& app)
{
    app.addSystem(AppSchedule::Render, "SceneSession.RenderScene", [](AppScheduleContext& ctx) {
        if (!ctx.gpuDevice)
            return;

        sceneSession::renderScene(ctx.app, *ctx.gpuDevice);
    });

    app.addSystemAfter(AppSchedule::Render, "SceneSession.AfterWorldRender", "SceneSession.RenderScene", [](AppScheduleContext& ctx) {
        if (!ctx.gpuDevice)
            return;

        sceneSession::afterWorldRenderScheduled(ctx.app, *ctx.gpuDevice);
    });
}

} // namespace caustica::sceneSession
