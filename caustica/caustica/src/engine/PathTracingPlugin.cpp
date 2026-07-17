#include <engine/ScenePlugins.h>

#include <engine/App.h>
#include <engine/AppSchedules.h>
#include <engine/SceneApi.h>

namespace caustica
{

void PathTracingPlugin::configureSchedules(App& app)
{
    app.addSystem(AppSchedule::render, "Scene.RenderScene", [](SystemContext& ctx) {
        if (!ctx.gpuDevice || !activeScene(ctx.app))
            return;

        renderScene(ctx.app, *ctx.gpuDevice);
    });

    app.addSystemAfter(AppSchedule::render, "Scene.AfterWorldRender", "Scene.RenderScene", [](SystemContext& ctx) {
        if (!ctx.gpuDevice)
            return;

        afterWorldRenderScheduled(ctx.app, *ctx.gpuDevice);
    });
}

} // namespace caustica
