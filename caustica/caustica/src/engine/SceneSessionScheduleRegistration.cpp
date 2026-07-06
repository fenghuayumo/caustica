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



    app.addSystemBefore(AppSchedule::Update, "SceneSession.BeginFrame", "NotifyDpiScale", [](AppScheduleContext& ctx) {

        sceneSession::beginFrameScheduled(ctx.app);

    });



    app.addSystemAfter(AppSchedule::Update, "SceneSession.Animate", "BeforeAnimate", [](AppScheduleContext& ctx) {

        if (!ctx.windowFocused)

            return;



        sceneSession::animate(ctx.app, ctx.deltaTimeSeconds);

    });



    app.addSystemBefore(AppSchedule::PostUpdate, "SceneSession.RefreshEntityWorld", "AfterAnimate", [](AppScheduleContext& ctx) {

        sceneSession::refreshEntityWorld(ctx.app, ctx.frameIndex);

    });



    app.addSystemAfter(AppSchedule::Extract, "SceneSession.PrepareRenderFrame", "SetRenderFrameIndex", [](AppScheduleContext& ctx) {

        if (!ctx.gpuDevice)

            return;



        if (sceneSession::shouldSkipRender(ctx.app))

            return;



        sceneSession::prepareRenderFrame(ctx.app);

    });



    app.addSystem(AppSchedule::Render, "SceneSession.RenderScene", [](AppScheduleContext& ctx) {

        if (!ctx.gpuDevice)

            return;



        sceneSession::renderScene(ctx.app, *ctx.gpuDevice);

    });



    app.addSystemAfter(AppSchedule::Render, "SceneSession.AfterWorldRender", "SceneSession.RenderScene", [](AppScheduleContext& ctx) {

        if (!ctx.gpuDevice)

            return;



        sceneSession::afterWorldRenderScheduled(ctx.app, *ctx.gpuDevice);

    });



    app.markSceneSessionSchedulesRegistered();

}



} // namespace caustica

