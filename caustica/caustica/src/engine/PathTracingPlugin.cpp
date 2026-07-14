#include <engine/SceneSessionPlugins.h>

#include <engine/App.h>
#include <engine/AppSchedules.h>
#include <engine/GpuRenderSubsystem.h>
#include <engine/SceneSessionSystems.h>

#include <scene/SceneManager.h>

namespace caustica::sceneSession
{

void registerPathTracingPlugin(App& app)
{
    app.addSystem(AppSchedule::render, "SceneSession.RenderScene", [](SystemContext& ctx) {
        if (!ctx.gpuDevice)
            return;

        auto* gr = ctx.tryRes<GpuRenderSubsystem>();
        if (!gr || !gr->sceneManager() || !gr->sceneManager()->getScene())
            return;

        renderScene(ctx.app, *ctx.gpuDevice);
    });

    app.addSystemAfter(AppSchedule::render, "SceneSession.AfterWorldRender", "SceneSession.RenderScene", [](SystemContext& ctx) {
        if (!ctx.gpuDevice)
            return;

        afterWorldRenderScheduled(ctx.app, *ctx.gpuDevice);
    });
}

} // namespace caustica::sceneSession
