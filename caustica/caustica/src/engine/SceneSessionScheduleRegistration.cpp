#include <engine/SceneSessionScheduleRegistration.h>



#include <engine/App.h>

#include <engine/AppSchedules.h>

#include <engine/SceneSessionSystems.h>

#include <engine/SceneViewState.h>



namespace caustica

{



void registerSceneSessionSchedules(App& app)

{

    if (app.sceneSessionSchedulesRegistered())

        return;



    if (!app.tryResource<SceneViewState>())

        return;



    app.addSystem(AppSchedule::PreUpdate, "SceneSession.BeginFrame", [](AppScheduleContext& ctx) {

        sceneSession::beginFrameScheduled(ctx.app);

    });



    app.addSystem(AppSchedule::Update, "SceneSession.Animate", [](AppScheduleContext& ctx) {

        if (!ctx.windowFocused)

            return;



        sceneSession::animate(ctx.app, ctx.deltaTimeSeconds);

    });



    app.addSystem(AppSchedule::PostUpdate, "SceneSession.RefreshEntityWorld", [](AppScheduleContext& ctx) {

        sceneSession::refreshEntityWorld(ctx.app, ctx.frameIndex);

    });



    app.addSystem(AppSchedule::PreRender, "SceneSession.PrepareRenderFrame", [](AppScheduleContext& ctx) {

        if (!ctx.gpuDevice)

            return;



        if (sceneSession::shouldSkipRender(ctx.app))

            return;



        sceneSession::prepareRenderFrame(ctx.app);

    });



    app.addSystem(AppSchedule::RenderScene, "SceneSession.RenderScene", [](AppScheduleContext& ctx) {

        if (!ctx.gpuDevice)

            return;



        sceneSession::renderScene(ctx.app, *ctx.gpuDevice);

    });



    app.markSceneSessionSchedulesRegistered();

}



} // namespace caustica

