#include "EditorPlugin.h"

#include <engine/App.h>
#include <engine/AssetPlugin.h>
#include <engine/SceneSessionSystems.h>

namespace caustica::editor
{

void EditorPlugin::build(App& app)
{
    registerAssetPlugin(app);
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

    m_sceneEditor.setApp(app);
    sessionConfig.hasSceneCallbacks = true;
    sessionConfig.sceneCallbacks = EngineSceneCallbacks{
        .OnSceneLoaded = [&app, this]() {
            m_sceneEditor.onSceneLoadedFromLoader();
        },
        .OnSceneUnloading = [&app, this]() {
            sceneSession::onSceneUnloading(app);
            m_sceneEditor.onSceneUnloading();
        },
    };

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
    app.addSystemBefore(AppSchedule::First, "EditorScene.beginFrame", "SceneSession.beginFrame", [](SystemContext& ctx) {
        auto* capture = ctx.tryRes<CaptureScriptState>();
        if (capture && capture->manager)
            capture->manager->preRender();
    });

    app.addSystemAfter(AppSchedule::preUpdate, "EditorScene.RequestUnfocusedRender", "NotifyDpiScale", [this](SystemContext& ctx) {
        if (ctx.runRender || !ctx.windowVisible || m_sceneEditor.shouldSkipRender())
            return;

        auto* wr = m_sceneEditor.worldRenderer();
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
        m_sceneEditor.ProcessPendingSceneMutations();
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

    app.addSystemAfter(AppSchedule::update, "EditorScene.SyncLoadedScene", "SceneSession.animate", [this](SystemContext& ctx) {
        (void)ctx;
        m_sceneEditor.syncLoadedSceneSystems();
    });

    app.addSystemAfter(AppSchedule::update, "EditorScene.AnimateEnd", "SceneSession.animate", [this](SystemContext& ctx) {
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
    editorUiRenderOrdering.after.push_back("SceneSession.AfterWorldRender");
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
