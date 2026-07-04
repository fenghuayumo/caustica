#include "EditorSceneSubsystem.h"

#include "SceneEditor.h"
#include "common/LocalConfig.h"

#include <core/path_utils.h>
#include <engine/GpuRenderSubsystem.h>
#include <engine/SubsystemCollection.h>
#include <render/Core/RenderSceneTypeFactory.h>
#include <render/RenderSessionState.h>
#include <render/SceneLightingPasses.h>
#include <render/WorldRenderer/WorldRenderer.h>

namespace caustica::editor
{

EditorSceneSubsystem::EditorSceneSubsystem(EditorSceneSubsystemConfig config)
    : m_config(std::move(config))
{
}

void EditorSceneSubsystem::initialize(caustica::EngineInitContext& context)
{
    if (!context.gpuDevice || !context.subsystems)
        return;

    auto* gpuRenderSubsystem = context.subsystems->get<caustica::GpuRenderSubsystem>();
    if (!gpuRenderSubsystem)
        return;

    SceneEditor& sceneEditor = m_config.sceneEditor;
    caustica::GpuDevice& gpuDevice = *context.gpuDevice;

    sceneEditor.setGpuDevice(gpuDevice);
    sceneEditor.initStreamlineAndWindow();
    if (context.application)
        sceneEditor.setApplication(context.application);

    SceneEditor* sceneEditorPtr = &sceneEditor;
    gpuRenderSubsystem->initializeSession(caustica::GpuRenderSubsystemInitParams{
        .gpuDevice = gpuDevice,
        .settings = sceneEditor.GetPathTracerSettings(),
        .runtimeState = sceneEditor.GetRenderRuntimeState(),
        .sceneTime = sceneEditor.GetSceneTimeRef(),
        .diagnostics = m_config.diagnostics,
        .cmdLine = m_config.cmdLine,
        .sceneTypeFactory = std::make_shared<caustica::render::RenderSceneTypeFactory>(),
        .sceneCallbacks = caustica::EngineSceneCallbacks{
            .OnSceneLoaded = [sceneEditorPtr]() { sceneEditorPtr->SceneLoaded(); },
            .OnSceneUnloading = [sceneEditorPtr]() { sceneEditorPtr->SceneUnloading(); },
        },
    });

    sceneEditor.bindGpuRenderSubsystem(*gpuRenderSubsystem);

    sceneEditor.Init(m_config.preferredScene, gpuRenderSubsystem->shaderFactory());

    if (m_config.refreshEnvMapMediaList)
    {
        gpuRenderSubsystem->lightingPasses().refreshEnvironmentMapMediaList(
            GetLocalPath(c_AssetsFolder), std::filesystem::path());
    }

    if (m_config.sessionState && m_config.cmdLine && m_config.applyCmdLineToSessionState)
        caustica::render::InitializeRenderSessionStateFromCommandLine(*m_config.sessionState, *m_config.cmdLine);

    if (m_config.sessionState && m_config.postAppInit)
        LocalConfig::PostAppInit(*m_config.sessionState);
}

void EditorSceneSubsystem::onBeginFrame(caustica::GpuDevice& /*gpuDevice*/)
{
    m_config.sceneEditor.CaptureScriptPreRender();
    m_config.sceneEditor.beginFrame();
}

void EditorSceneSubsystem::onUpdate(float elapsedTimeSeconds, bool windowFocused)
{
    if (windowFocused)
        m_config.sceneEditor.Animate(elapsedTimeSeconds);
}

void EditorSceneSubsystem::onRenderScene(caustica::GpuDevice& gpuDevice)
{
    SceneEditor& editor = m_config.sceneEditor;
    if (editor.shouldSkipRender())
        return;

    editor.PrepareEditorFrame();

    auto* worldRenderer = editor.GetWorldRenderer();
    if (!worldRenderer)
        return;

    worldRenderer->render(gpuDevice.GetCurrentFramebuffer(true));
    editor.afterWorldRender(gpuDevice);
    editor.recordFrameTiming(gpuDevice);
}

void EditorSceneSubsystem::onBackBufferResizing()
{
    m_config.sceneEditor.BackBufferResizing();
}

bool EditorSceneSubsystem::skipRenderPhase() const
{
    return m_config.sceneEditor.shouldSkipRender();
}

bool EditorSceneSubsystem::shouldRenderWhenUnfocused() const
{
    return m_config.sceneEditor.ShouldRenderUnfocused();
}

} // namespace caustica::editor
