#include "EditorPlugin.h"
#include "EditorAccess.h"
#include "common/CaptureScriptManager.h"

#include <engine/App.h>
#include <engine/SceneQuery.h>
#include <engine/SceneLifecycle.h>
#include <render/worldRenderer/WorldRenderer.h>

namespace caustica::editor
{

void EditorPlugin::build(App& app)
{
    defaults.build(app);

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
    defaults.appConfig.hasSceneCallbacks = true;
    defaults.appConfig.sceneCallbacks = EngineSceneCallbacks{
        .OnSceneLoaded = [this]() {
            m_sceneEditor.onSceneLoadedFromLoader();
        },
        .OnSceneUnloading = [&app, this]() {
            caustica::onSceneUnloading(app);
            m_sceneEditor.onSceneUnloading();
        },
    };

    defaults.configureSchedules(app);
    registerEditorSceneStartup(app, EditorSceneStartupConfig{
        .appConfig = defaults.appConfig,
        .sceneEditor = &m_sceneEditor,
    });

    if (uiConfig)
        registerEditorUISubsystemLifecycle(app);
}

void EditorPlugin::configureLateSchedules(App& app)
{
    app.addSystemBefore(AppSchedule::First, "EditorScene.beginFrame", "Scene.beginFrame", [](SystemContext& ctx) {
        auto* capture = ctx.tryRes<CaptureScriptState>();
        if (capture && capture->manager)
            capture->manager->preRender();
    });

    app.addSystemAfter(AppSchedule::preUpdate, "EditorScene.RequestUnfocusedRender", "NotifyDpiScale", [this](SystemContext& ctx) {
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

    app.addSystemAfter(AppSchedule::preUpdate, "EditorScene.ProcessPendingMutations", "BeforeAnimate", [this](SystemContext& ctx) {
        (void)ctx;
        m_sceneEditor.processPendingSceneDeletes();
    });

    app.addSystemAfter(AppSchedule::preUpdate, "EditorScene.AnimateBegin", "EditorScene.ProcessPendingMutations", [this](SystemContext& ctx) {
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

    app.addSystemAfter(AppSchedule::update, "EditorScene.SyncLoadedScene", "Scene.animate", [this](SystemContext& ctx) {
        (void)ctx;
        m_sceneEditor.syncLoadedSceneSystems();
    });

    app.addSystemAfter(AppSchedule::update, "EditorScene.AnimateEnd", "Scene.animate", [this](SystemContext& ctx) {
        if (!ctx.windowFocused)
            return;

        m_sceneEditor.onAnimateGameCamera(ctx.deltaTimeSeconds);
        m_sceneEditor.onAnimateEnd(ctx.deltaTimeSeconds);
        m_sceneEditor.updateWindowTitle();
    });

    app.addSystem(AppSchedule::PostUpdate, "EditorScene.handleDroppedFiles", [this](SystemContext& ctx) {
        (void)ctx;
        // Import before Extract so PrepareRenderFrame sees the final ECS graph and
        // the render phase does not race a mid-Extract snapshot overwrite.
        m_sceneEditor.handleDroppedFiles();
    });

    app.addSystemAfter(AppSchedule::Extract, "EditorScene.prepareEditorFrame", "Scene.PrepareRenderFrame", [this](SystemContext& ctx) {
        if (!ctx.gpuDevice || caustica::shouldSkipRender(*m_sceneEditor.app()))
            return;

        m_sceneEditor.prepareEditorFrame();
    });

    AppSystemOrdering editorAfterWorldRenderOrdering;
    editorAfterWorldRenderOrdering.before.push_back("Scene.AfterWorldRender");
    editorAfterWorldRenderOrdering.before.push_back("GpuRender.endFrame");

    app.addSystem(AppSchedule::render, "EditorScene.AfterWorldRender", [this](SystemContext& ctx) {
        if (!ctx.gpuDevice)
            return;

        m_sceneEditor.afterWorldRender(*ctx.gpuDevice);
    }, std::move(editorAfterWorldRenderOrdering));

    if (!uiConfig)
        return;

    app.addSystemAfter(AppSchedule::update, "EditorUI.animate", "EditorScene.AnimateEnd", [](SystemContext& ctx) {
        auto* uiSubsystem = ctx.tryRes<EditorUISubsystem>();
        if (!uiSubsystem)
            return;

        uiSubsystem->animateScheduled(ctx.deltaTimeSeconds, ctx.windowFocused);
    });

    AppSystemOrdering editorUiRenderOrdering;
    editorUiRenderOrdering.after.push_back("Scene.AfterWorldRender");
    editorUiRenderOrdering.before.push_back("GpuRender.endFrame");

    app.addSystem(AppSchedule::render, "EditorUI.RenderScene", [](SystemContext& ctx) {
        if (!ctx.gpuDevice)
            return;

        auto* uiSubsystem = ctx.tryRes<EditorUISubsystem>();
        if (!uiSubsystem)
            return;

        uiSubsystem->renderSceneScheduled(*ctx.gpuDevice);
    }, std::move(editorUiRenderOrdering));
}

} // namespace caustica::editor
