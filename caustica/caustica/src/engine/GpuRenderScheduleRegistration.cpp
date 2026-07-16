#include <engine/GpuRenderScheduleRegistration.h>
#include <engine/SceneStartup.h>

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
    ordering.after.push_back("Scene.RenderScene");
    ordering.after.push_back("Scene.AfterWorldRender");

    app.addSystem(AppSchedule::render, "GpuRender.endFrame", [](ResMut<GpuRenderSubsystem> gpuRender) {
        gpuRender->endFrame();
    }, std::move(ordering));

    registerGpuRenderShutdown(app);

    app.markGpuRenderSchedulesRegistered();
}

} // namespace caustica
