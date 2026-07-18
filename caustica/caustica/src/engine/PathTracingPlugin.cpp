#include <engine/ScenePlugins.h>

#include <engine/App.h>
#include <engine/AppSchedules.h>
#include <engine/SceneQuery.h>
#include <engine/RenderFrameApi.h>
#include <engine/SystemLabels.h>

namespace caustica
{

void PathTracingPlugin::configureSchedules(App& app)
{
    app.addSystem<system_label::SceneRenderScene>(AppSchedule::render, [](SystemContext& ctx) {
        if (!ctx.gpuDevice || !activeScene(ctx.app))
            return;

        renderScene(ctx.app, *ctx.gpuDevice);
    });

    app.addSystemAfter<system_label::SceneAfterWorldRender, system_label::SceneRenderScene>(
        AppSchedule::render,
        [](SystemContext& ctx) {
            if (!ctx.gpuDevice)
                return;

            afterWorldRenderScheduled(ctx.app, *ctx.gpuDevice);
        });
}

} // namespace caustica
