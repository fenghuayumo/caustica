#include <engine/SceneSessionPlugins.h>

#include <engine/App.h>
#include <engine/AppSchedules.h>
#include <engine/SceneSessionSystems.h>

namespace caustica::sceneSession
{

void registerPathTracingPlugin(App& app)
{
    app.addSystem(AppSchedule::render, "SceneSession.RenderScene", [](SystemContext& ctx) {
        if (!ctx.gpuDevice)
            return;

        sceneSession::renderScene(ctx.app, *ctx.gpuDevice);
    });

    app.addSystemAfter(AppSchedule::render, "SceneSession.AfterWorldRender", "SceneSession.RenderScene", [](SystemContext& ctx) {
        if (!ctx.gpuDevice)
            return;

        sceneSession::afterWorldRenderScheduled(ctx.app, *ctx.gpuDevice);
    });
}

} // namespace caustica::sceneSession
