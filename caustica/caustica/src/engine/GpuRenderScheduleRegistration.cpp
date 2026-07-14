#include <engine/GpuRenderScheduleRegistration.h>
#include <engine/SceneSessionStartup.h>

#include <engine/App.h>
#include <engine/AppSchedules.h>
#include <engine/GpuRenderSubsystem.h>

#include <utility>

namespace caustica
{

void registerGpuRenderSchedules(App& app)
{
    if (app.gpuRenderSchedulesRegistered())
        return;

    if (!app.tryResource<GpuRenderSubsystem>())
        return;

    AppSystemOrdering ordering;
    ordering.after.push_back("SceneSession.RenderScene");
    ordering.after.push_back("SceneSession.AfterWorldRender");

    app.addSystem(AppSchedule::render, "GpuRender.endFrame", [](SystemContext& ctx) {
        if (auto* gpuRender = ctx.tryRes<GpuRenderSubsystem>())
            gpuRender->endFrame();
    }, std::move(ordering));

    registerGpuRenderShutdown(app);

    app.markGpuRenderSchedulesRegistered();
}

} // namespace caustica
