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

    app.addSystem(AppSchedule::Render, "GpuRender.EndFrame", [&app](AppScheduleContext& ctx) {
        (void)ctx;
        if (auto* gpuRender = app.tryResource<GpuRenderSubsystem>())
            gpuRender->endFrame();
    }, std::move(ordering));

    registerGpuRenderShutdown(app);

    app.markGpuRenderSchedulesRegistered();
}

} // namespace caustica
