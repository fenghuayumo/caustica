#include <engine/GpuRenderScheduleRegistration.h>
#include <engine/SceneStartup.h>

#include <engine/App.h>
#include <engine/AppSchedules.h>
#include <render/worldRenderer/WorldRenderer.h>
#include <engine/GpuSharedCaches.h>

#include <utility>

namespace caustica
{

void registerGpuRenderSchedules(App& app)
{
    if (app.gpuRenderSchedulesRegistered())
        return;

    if (!app.tryResource<render::WorldRenderer>() || !app.tryResource<GpuSharedCaches>())
        return;

    AppSystemOrdering ordering;
    ordering.after.push_back("Scene.RenderScene");
    ordering.after.push_back("Scene.AfterWorldRender");

    app.addSystem(AppSchedule::render, "GpuRender.endFrame", [](ResMut<GpuSharedCaches> caches) {
        caches->endFrame();
    }, std::move(ordering));

    registerGpuRenderShutdown(app);

    app.markGpuRenderSchedulesRegistered();
}

} // namespace caustica
