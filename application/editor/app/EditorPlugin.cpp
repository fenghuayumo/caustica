#include "EditorPlugin.h"
#include "EditorAccess.h"
#include "EditorSystemLabels.h"
#include "common/CaptureScriptManager.h"

#include <engine/App.h>
#include <engine/SceneQuery.h>
#include <engine/SceneLifecycle.h>
#include <engine/SystemLabels.h>
#include <render/WorldRenderer.h>

namespace caustica::editor
{

void EditorPlugin::build(App& app)
{
    app.insertResourceRef(m_sceneEditor);
    app.insertResourceRef(m_sceneEditor.editorState());
    app.insertResourceRef(m_sceneEditor.captureScriptState());
    app.insertResourceRef(m_sceneEditor.selectionState());
    app.insertResourceRef(m_sceneEditor.editorCameraState());
    if (m_sceneEditor.hasEditorUiData())
        app.insertResourceRef(m_sceneEditor.uiData());

    if (uiConfig)
        app.emplaceResource<EditorUISubsystem>(*uiConfig);
}

void EditorPlugin::configureSchedules(App& app)
{
    m_sceneEditor.setApp(app);

    // Scene.Startup callbacks are owned by EngineAppDesc / SceneRuntimePlugin.
    // EditorPlugin only registers editor Pre/Post startup around that system.
    registerEditorSceneStartup(app, EditorSceneStartupConfig{
        .appConfig = appConfig,
        .sceneEditor = &m_sceneEditor,
    });

    if (uiConfig)
        registerEditorUISubsystemLifecycle(app);
}

void EditorPlugin::configureLateSchedules(App& app)
{
    app.addSystemBefore<system_label::EditorSceneBeginFrame, caustica::system_label::SceneBeginFrame>(
        AppSchedule::First,
        [](SystemContext& ctx) {
            auto* capture = ctx.tryRes<CaptureScriptState>();
            if (capture && capture->manager)
                capture->manager->preRender();
        });

    app.addSystemAfter<system_label::EditorSceneRequestUnfocusedRender, caustica::system_label::NotifyDpiScale>(
        AppSchedule::preUpdate,
        [this](SystemContext& ctx) {
            if (ctx.runRender || !ctx.windowVisible || caustica::shouldSkipRender(*m_sceneEditor.app()))
                return;

            auto* wr = caustica::editor::editorWorldRenderer(m_sceneEditor);
            auto* ui = ctx.tryRes<EditorUiData>();
            auto* capture = ctx.tryRes<CaptureScriptState>();
            const auto& settings = m_sceneEditor.pathTracerSettings();

            const bool captureActive = capture && capture->manager && capture->manager->isDoingWork();
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

    app.addSystemAfter<system_label::EditorSceneProcessPendingMutations, caustica::system_label::BeforeAnimate>(
        AppSchedule::preUpdate,
        [this](SystemContext& ctx) {
            (void)ctx;
            m_sceneEditor.processPendingSceneDeletes();
        });

    app.addSystemAfter<system_label::EditorSceneAnimateBegin, system_label::EditorSceneProcessPendingMutations>(
        AppSchedule::preUpdate,
        [this](SystemContext& ctx) {
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

    app.addSystemAfter<system_label::EditorSceneSyncLoadedScene, caustica::system_label::SceneAnimate>(
        AppSchedule::update,
        [this](SystemContext& ctx) {
            (void)ctx;
            m_sceneEditor.syncLoadedSceneSystems();
        });

    app.addSystemAfter<system_label::EditorSceneAnimateEnd, caustica::system_label::SceneAnimate>(
        AppSchedule::update,
        [this](SystemContext& ctx) {
            if (!ctx.windowFocused)
                return;

            m_sceneEditor.onAnimateGameCamera(ctx.deltaTimeSeconds);
            m_sceneEditor.onAnimateEnd(ctx.deltaTimeSeconds);
            m_sceneEditor.updateWindowTitle();
        });

    app.addSystem<system_label::EditorSceneHandleDroppedFiles>(AppSchedule::PostUpdate, [this](SystemContext& ctx) {
        (void)ctx;
        // Import before Extract so PrepareRenderFrame sees the final ECS graph and
        // the render phase does not race a mid-Extract snapshot overwrite.
        m_sceneEditor.handleDroppedFiles();
    });

    app.addSystemAfter<system_label::EditorScenePrepareEditorFrame, caustica::system_label::ScenePrepareRenderFrame>(
        AppSchedule::Extract,
        [this](SystemContext& ctx) {
            if (!ctx.gpuDevice || caustica::shouldSkipRender(*m_sceneEditor.app()))
                return;

            m_sceneEditor.prepareEditorFrame();
        });

    app.addSystem<system_label::EditorSceneAfterWorldRender>(
        AppSchedule::render,
        [this](SystemContext& ctx) {
            if (!ctx.gpuDevice)
                return;

            m_sceneEditor.afterWorldRender(*ctx.gpuDevice);
        },
        AppSystemOrdering{}
            .runBefore<caustica::system_label::SceneAfterWorldRender, caustica::system_label::GpuRenderEndFrame>());

    if (!uiConfig)
        return;

    app.addSystemAfter<system_label::EditorUIAnimate, system_label::EditorSceneAnimateEnd>(
        AppSchedule::update,
        [](SystemContext& ctx) {
            auto* uiSubsystem = ctx.tryRes<EditorUISubsystem>();
            if (!uiSubsystem)
                return;

            uiSubsystem->animateScheduled(ctx.deltaTimeSeconds, ctx.windowFocused);
        });

    app.addSystem<system_label::EditorUIRenderScene>(
        AppSchedule::render,
        [](SystemContext& ctx) {
            if (!ctx.gpuDevice)
                return;

            auto* uiSubsystem = ctx.tryRes<EditorUISubsystem>();
            if (!uiSubsystem)
                return;

            uiSubsystem->renderSceneScheduled(*ctx.gpuDevice);
        },
        AppSystemOrdering{}
            .runAfter<caustica::system_label::SceneAfterWorldRender>()
            .runBefore<caustica::system_label::GpuRenderEndFrame>());
}

} // namespace caustica::editor
