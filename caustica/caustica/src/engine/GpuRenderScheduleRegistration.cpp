#include <engine/GpuRenderScheduleRegistration.h>

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

    if (!app.getSubsystem<GpuRenderSubsystem>())
        return;

    AppSystemOrdering ordering;
    ordering.after.push_back("SceneSession.RenderScene");
    ordering.after.push_back("SceneSession.AfterWorldRender");

    app.addSystem(AppSchedule::Render, "GpuRender.EndFrame", [&app](AppScheduleContext& ctx) {
        (void)ctx;
        if (auto* gpuRender = app.getSubsystem<GpuRenderSubsystem>())
            gpuRender->endFrame();
    }, std::move(ordering));

    app.markGpuRenderSchedulesRegistered();
}

} // namespace caustica
