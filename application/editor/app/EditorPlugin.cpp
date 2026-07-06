#include "EditorPlugin.h"

#include <engine/App.h>

namespace caustica::editor
{

void EditorPlugin::build(App& app)
{
    registerSceneSessionResources(app, sessionConfig);
    app.insertResourceRef(m_sceneEditor);
    app.insertResourceRef(m_sceneEditor.editorState());
    app.insertResourceRef(m_sceneEditor.captureScriptState());
    app.insertResourceRef(m_sceneEditor.selectionState());
    app.insertResourceRef(m_sceneEditor.editorCameraState());
    if (m_sceneEditor.hasEditorUiData())
        app.insertResourceRef(m_sceneEditor.GetUIData());

    app.emplaceResource<GpuRenderSubsystem>();

    if (uiConfig)
        app.emplaceResource<EditorUISubsystem>(*uiConfig);
}

void EditorPlugin::configureSchedules(App& app)
{
    registerEngineScheduleBridge(app);
    registerSceneSessionStartup(app, sessionConfig);
    registerEditorSceneStartup(app, EditorSceneStartupConfig{
        .session = sessionConfig,
        .sceneEditor = &m_sceneEditor,
    });

    if (uiConfig)
        registerEditorUISubsystemLifecycle(app);
}

void EditorPlugin::configureLateSchedules(App& app)
{
    app.addSystemBefore(AppSchedule::First, "EditorScene.BeginFrame", "SceneSession.BeginFrame", [](SystemContext& ctx) {
        auto* capture = ctx.tryRes<CaptureScriptState>();
        if (capture && capture->manager)
            capture->manager->PreRender();
    });

    app.addSystemAfter(AppSchedule::PreUpdate, "EditorScene.RequestUnfocusedRender", "NotifyDpiScale", [this](SystemContext& ctx) {
        if (ctx.runRender || !ctx.windowVisible || m_sceneEditor.shouldSkipRender())
            return;

        auto* wr = m_sceneEditor.worldRenderer();
        auto* ui = ctx.tryRes<EditorUiData>();
        auto* capture = ctx.tryRes<CaptureScriptState>();
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

    app.addSystemAfter(AppSchedule::PreUpdate, "EditorScene.AnimateBegin", "BeforeAnimate", [this](SystemContext& ctx) {
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

    app.addSystemAfter(AppSchedule::Update, "EditorScene.SyncLoadedScene", "SceneSession.Animate", [this](SystemContext& ctx) {
        (void)ctx;
        m_sceneEditor.syncLoadedSceneSystems();
    });

    app.addSystemAfter(AppSchedule::Update, "EditorScene.AnimateEnd", "SceneSession.Animate", [this](SystemContext& ctx) {
        if (!ctx.windowFocused)
            return;

        m_sceneEditor.onAnimateGameCamera(ctx.deltaTimeSeconds);
        m_sceneEditor.onAnimateEnd(ctx.deltaTimeSeconds);
        m_sceneEditor.updateWindowTitle();
    });

    app.addSystemAfter(AppSchedule::Extract, "EditorScene.PrepareEditorFrame", "SceneSession.PrepareRenderFrame", [this](SystemContext& ctx) {
        if (!ctx.gpuDevice || m_sceneEditor.shouldSkipRender())
            return;

        m_sceneEditor.PrepareEditorFrame();
    });

    AppSystemOrdering editorAfterWorldRenderOrdering;
    editorAfterWorldRenderOrdering.before.push_back("SceneSession.AfterWorldRender");
    editorAfterWorldRenderOrdering.before.push_back("GpuRender.EndFrame");

    app.addSystem(AppSchedule::Render, "EditorScene.AfterWorldRender", [this](SystemContext& ctx) {
        if (!ctx.gpuDevice)
            return;

        m_sceneEditor.afterWorldRender(*ctx.gpuDevice);
    }, std::move(editorAfterWorldRenderOrdering));

    if (!uiConfig)
        return;

    app.addSystemAfter(AppSchedule::Update, "EditorUI.Animate", "EditorScene.AnimateEnd", [](SystemContext& ctx) {
        auto* uiSubsystem = ctx.tryRes<EditorUISubsystem>();
        if (!uiSubsystem)
            return;

        uiSubsystem->animateScheduled(ctx.deltaTimeSeconds, ctx.windowFocused);
    });

    AppSystemOrdering editorUiRenderOrdering;
    editorUiRenderOrdering.after.push_back("SceneSession.AfterWorldRender");
    editorUiRenderOrdering.before.push_back("GpuRender.EndFrame");

    app.addSystem(AppSchedule::Render, "EditorUI.RenderScene", [](SystemContext& ctx) {
        if (!ctx.gpuDevice)
            return;

        auto* uiSubsystem = ctx.tryRes<EditorUISubsystem>();
        if (!uiSubsystem)
            return;

        uiSubsystem->renderSceneScheduled(*ctx.gpuDevice);
    }, std::move(editorUiRenderOrdering));
}

} // namespace caustica::editor
