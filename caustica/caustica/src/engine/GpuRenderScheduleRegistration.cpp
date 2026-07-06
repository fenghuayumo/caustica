#include <engine/GpuRenderScheduleRegistration.h>

#include <engine/App.h>
#include <engine/AppSchedules.h>
#include <engine/GpuRenderSubsystem.h>

namespace caustica
{

void registerGpuRenderSchedules(App& app)
{
    if (app.gpuRenderSchedulesRegistered())
        return;

    if (!app.getSubsystem<GpuRenderSubsystem>())
        return;

    app.addSystem(AppSchedule::RenderFinalize, "GpuRender.EndFrame", [&app](AppScheduleContext& ctx) {
        (void)ctx;
        if (auto* gpuRender = app.getSubsystem<GpuRenderSubsystem>())
            gpuRender->endFrame();
    });

    app.markGpuRenderSchedulesRegistered();
}

} // namespace caustica
