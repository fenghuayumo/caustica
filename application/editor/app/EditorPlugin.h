#pragma once



#include <engine/App.h>

#include <engine/EngineScheduleRegistration.h>

#include <engine/SceneSessionResources.h>

#include <engine/GpuRenderSubsystem.h>

#include <engine/Plugin.h>

#include <engine/SceneSessionSubsystem.h>

#include <render/worldRenderer/WorldRenderer.h>



#include "EditorSceneSubsystem.h"

#include "EditorUISubsystem.h"

#include "common/CaptureScriptManager.h"

#include "SceneEditor.h"



#include <imgui/imgui_renderer.h>



namespace caustica::editor

{



struct EditorPlugin : Plugin

{

    EditorPlugin(SceneSessionConfig sessionConfig,

        SceneEditor& sceneEditor,

        const EditorUISubsystemConfig* uiConfig = nullptr)

        : sessionConfig(std::move(sessionConfig))

        , m_sceneEditor(sceneEditor)

        , uiConfig(uiConfig)

    {

    }



    void build(App& app) override

    {

        registerSceneSessionResources(app, sessionConfig);

        app.insertResourceRef(m_sceneEditor);
        app.insertResourceRef(m_sceneEditor.editorState());
        app.insertResourceRef(m_sceneEditor.captureScriptState());
        app.insertResourceRef(m_sceneEditor.selectionState());
        app.insertResourceRef(m_sceneEditor.editorCameraState());
        if (m_sceneEditor.hasEditorUiData())
            app.insertResourceRef(m_sceneEditor.GetUIData());

        app.emplaceSubsystem<GpuRenderSubsystem>();

        app.emplaceSubsystem<EditorSceneSubsystem>(EditorSceneSubsystemConfig{

            .session = sessionConfig,

            .sceneEditor = &m_sceneEditor,

        });



        if (uiConfig)

            app.emplaceSubsystem<EditorUISubsystem>(*uiConfig);

    }



    void configureSchedules(App& app) override

    {

        registerEngineScheduleBridge(app);

    }



    void configureLateSchedules(App& app) override

    {

        app.addSystemBefore(AppSchedule::First, "EditorScene.BeginFrame", "SceneSession.BeginFrame", [](AppScheduleContext& ctx) {

            auto* capture = ctx.app.tryResource<CaptureScriptState>();
            if (capture && capture->manager)
                capture->manager->PreRender();


        });



        app.addSystemAfter(AppSchedule::PreUpdate, "EditorScene.RequestUnfocusedRender", "NotifyDpiScale", [this](AppScheduleContext& ctx) {

            if (ctx.runRender || !ctx.windowVisible || m_sceneEditor.shouldSkipRender())

                return;



            auto* wr = m_sceneEditor.worldRenderer();
            auto* ui = ctx.app.tryResource<EditorUiData>();
            auto* capture = ctx.app.tryResource<CaptureScriptState>();
            const auto& settings = m_sceneEditor.pathTracerSettings();

            const bool captureActive = capture && capture->manager && capture->manager->IsDoingWork();
            const bool editorRequestsRender = ui && ui->editor.RenderWhenOutOfFocus;
            const bool accumulationIncomplete = wr
                && !settings.RealtimeMode
                && wr->getAccumulationSampleIndex() < settings.AccumulationTarget;

            if (!wr || (
                wr->getFrameIndex() >= 16
                && !settings.ResetAccumulation
                && !settings.ResetRealtimeCaches
                && !captureActive
                && !editorRequestsRender
                && !accumulationIncomplete))

                return;



            ctx.runRender = true;

            ctx.windowFocused = true;

            ctx.app.requestRenderUnfocused();

        });



        app.addSystemAfter(AppSchedule::PreUpdate, "EditorScene.AnimateBegin", "BeforeAnimate", [this](AppScheduleContext& ctx) {

            if (!ctx.windowFocused)

                return;



            m_sceneEditor.onAnimateBegin(ctx.deltaTimeSeconds);

            ctx.elapsedTime = ctx.deltaTimeSeconds;

            const auto& settings = m_sceneEditor.pathTracerSettings();

            const bool enableAnimations = settings.EnableAnimations && settings.RealtimeMode;

            const bool enableAnimationUpdate = enableAnimations || settings.ResetAccumulation;

            m_sceneEditor.onAnimateGameTick(ctx.deltaTimeSeconds, enableAnimations);

            m_sceneEditor.onAnimateUpdateSceneTime(ctx.deltaTimeSeconds, enableAnimations, enableAnimationUpdate);

        });



        app.addSystemAfter(AppSchedule::Update, "EditorScene.SyncLoadedScene", "SceneSession.Animate", [this](AppScheduleContext& ctx) {

            (void)ctx;

            m_sceneEditor.syncLoadedSceneSystems();

        });



        app.addSystemAfter(AppSchedule::Update, "EditorScene.AnimateEnd", "SceneSession.Animate", [this](AppScheduleContext& ctx) {

            if (!ctx.windowFocused)

                return;



            m_sceneEditor.onAnimateGameCamera(ctx.deltaTimeSeconds);

            m_sceneEditor.onAnimateEnd(ctx.deltaTimeSeconds);

            m_sceneEditor.updateWindowTitle();

        });



        app.addSystemAfter(AppSchedule::Extract, "EditorScene.PrepareEditorFrame", "SceneSession.PrepareRenderFrame", [this](AppScheduleContext& ctx) {

            if (!ctx.gpuDevice || m_sceneEditor.shouldSkipRender())

                return;



            m_sceneEditor.PrepareEditorFrame();

        });



        AppSystemOrdering editorAfterWorldRenderOrdering;
        editorAfterWorldRenderOrdering.before.push_back("SceneSession.AfterWorldRender");
        editorAfterWorldRenderOrdering.before.push_back("GpuRender.EndFrame");

        app.addSystem(AppSchedule::Render, "EditorScene.AfterWorldRender", [this](AppScheduleContext& ctx) {

            if (!ctx.gpuDevice)

                return;



            m_sceneEditor.afterWorldRender(*ctx.gpuDevice);

        }, std::move(editorAfterWorldRenderOrdering));



        if (!uiConfig)

            return;



        app.addSystemAfter(AppSchedule::Update, "EditorUI.Animate", "EditorScene.AnimateEnd", [&app](AppScheduleContext& ctx) {

            auto* uiSubsystem = app.getSubsystem<EditorUISubsystem>();

            if (!uiSubsystem)

                return;



            uiSubsystem->animateScheduled(ctx.deltaTimeSeconds, ctx.windowFocused);

        });



        AppSystemOrdering editorUiRenderOrdering;
        editorUiRenderOrdering.after.push_back("SceneSession.AfterWorldRender");
        editorUiRenderOrdering.before.push_back("GpuRender.EndFrame");

        app.addSystem(AppSchedule::Render, "EditorUI.RenderScene", [&app](AppScheduleContext& ctx) {

            if (!ctx.gpuDevice)

                return;



            auto* uiSubsystem = app.getSubsystem<EditorUISubsystem>();

            if (!uiSubsystem)

                return;



            uiSubsystem->renderSceneScheduled(*ctx.gpuDevice);

        }, std::move(editorUiRenderOrdering));

    }



    SceneSessionConfig sessionConfig;

    SceneEditor& m_sceneEditor;

    const EditorUISubsystemConfig* uiConfig = nullptr;

};



} // namespace caustica::editor

