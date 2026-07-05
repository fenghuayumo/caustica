#include <engine/SubsystemScheduleRegistration.h>

#include <engine/App.h>
#include <engine/AppSchedules.h>
#include <engine/Engine.h>
#include <engine/ISubsystem.h>
#include <engine/SceneRuntimeSubsystem.h>

#include <string>

namespace caustica
{

void registerSubsystemSchedules(App& app)
{
    if (app.subsystemSchedulesRegistered())
        return;

    app.engine().subsystems().forEach([&app](ISubsystem& subsystem) {
        ISubsystem* raw = &subsystem;
        const std::string prefix(subsystem.scheduleLabel());

        app.addSystem(AppSchedule::PreUpdate, prefix + ".BeginFrame", [raw](AppScheduleContext& ctx) {
            if (ctx.gpuDevice)
                raw->onBeginFrame(*ctx.gpuDevice);
        });

        app.addSystem(AppSchedule::Update, prefix + ".Update", [raw](AppScheduleContext& ctx) {
            raw->onUpdate(ctx.deltaTimeSeconds, ctx.windowFocused);
        });

        app.addSystem(AppSchedule::PreRender, prefix + ".PrepareRenderScene", [raw](AppScheduleContext& ctx) {
            if (ctx.gpuDevice)
                raw->onPrepareRenderScene(*ctx.gpuDevice);
        });

        app.addSystem(AppSchedule::RenderScene, prefix + ".RenderScene", [raw](AppScheduleContext& ctx) {
            if (ctx.gpuDevice)
                raw->onRenderScene(*ctx.gpuDevice);
        });

        app.addSystem(AppSchedule::RenderFinalize, prefix + ".RenderEnd", [raw](AppScheduleContext& ctx) {
            if (ctx.gpuDevice)
                raw->onRenderEnd(*ctx.gpuDevice);
        });

        if (auto* sceneRuntime = dynamic_cast<SceneRuntimeSubsystem*>(raw))
        {
            app.addSystem(AppSchedule::PostUpdate, prefix + ".RefreshEntityWorld", [sceneRuntime](AppScheduleContext& ctx) {
                sceneRuntime->postUpdateSceneEntityWorld(ctx.frameIndex);
            });
        }
    });

    app.markSubsystemSchedulesRegistered();
}

} // namespace caustica
