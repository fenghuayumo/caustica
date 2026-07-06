#pragma once



#include <engine/App.h>

#include <engine/EngineScheduleRegistration.h>

#include <engine/SceneSessionResources.h>

#include <engine/GpuRenderSubsystem.h>

#include <engine/Plugin.h>

#include <engine/SceneSessionSubsystem.h>



#include "EditorSceneSubsystem.h"

#include "EditorUISubsystem.h"

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

        app.addSystemBefore(AppSchedule::Update, "EditorScene.BeginFrame", "SceneSession.BeginFrame", [this](AppScheduleContext& ctx) {

            (void)ctx;

            m_sceneEditor.onBeginFrameScheduled();

        });



        app.addSystemAfter(AppSchedule::Update, "EditorScene.RequestUnfocusedRender", "NotifyDpiScale", [this](AppScheduleContext& ctx) {

            if (ctx.runRender || !ctx.windowVisible || m_sceneEditor.shouldSkipRender())

                return;



            if (!m_sceneEditor.shouldRenderWhenUnfocused())

                return;



            ctx.runRender = true;

            ctx.windowFocused = true;

            ctx.app.requestRenderUnfocused();

        });



        app.addSystemBefore(AppSchedule::Update, "EditorScene.AnimateBegin", "SceneSession.Animate", [this](AppScheduleContext& ctx) {

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

