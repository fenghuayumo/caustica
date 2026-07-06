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

        app.addSystem(AppSchedule::PreRender, "EditorScene.PrepareEditorFrame", [this](AppScheduleContext& ctx) {

            if (!ctx.gpuDevice || m_sceneEditor.shouldSkipRender())

                return;



            m_sceneEditor.PrepareEditorFrame();

        });



        if (!uiConfig)

            return;



        app.addSystem(AppSchedule::Update, "EditorUI.Animate", [&app](AppScheduleContext& ctx) {

            auto* uiSubsystem = app.getSubsystem<EditorUISubsystem>();

            if (!uiSubsystem)

                return;



            uiSubsystem->animateScheduled(ctx.deltaTimeSeconds, ctx.windowFocused);

        });



        app.addSystem(AppSchedule::RenderScene, "EditorUI.RenderScene", [&app](AppScheduleContext& ctx) {

            if (!ctx.gpuDevice)

                return;



            auto* uiSubsystem = app.getSubsystem<EditorUISubsystem>();

            if (!uiSubsystem)

                return;



            uiSubsystem->renderSceneScheduled(*ctx.gpuDevice);

        });

    }



    SceneSessionConfig sessionConfig;

    SceneEditor& m_sceneEditor;

    const EditorUISubsystemConfig* uiConfig = nullptr;

};



} // namespace caustica::editor

