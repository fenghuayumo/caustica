#include <engine/GpuRenderScheduleRegistration.h>
#include <engine/SceneStartup.h>

#include <engine/App.h>
#include <engine/AppSchedules.h>
#include <engine/SystemLabels.h>
#include <render/WorldRenderer.h>
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

    app.addSystem<system_label::GpuRenderEndFrame>(
        AppSchedule::render,
        [](ResMut<GpuSharedCaches> caches) {
            caches->endFrame();
        },
        AppSystemOrdering{}.runAfter<system_label::SceneRenderScene, system_label::SceneAfterWorldRender>());

    registerGpuRenderShutdown(app);

    app.markGpuRenderSchedulesRegistered();
}

} // namespace caustica
