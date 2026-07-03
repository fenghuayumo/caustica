#include "EditorSceneSubsystem.h"

#include "SceneEditor.h"
#include "common/LocalConfig.h"

#include <core/path_utils.h>
#include <engine/RenderingSubsystem.h>
#include <engine/SubsystemCollection.h>
#include <render/Core/RenderSceneTypeFactory.h>
#include <render/RenderSessionState.h>
#include <render/SceneLightingPasses.h>

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

    auto* rendering = context.subsystems->get<caustica::RenderingSubsystem>();
    if (!rendering)
        return;

    SceneEditor& sceneEditor = m_config.sceneEditor;
    caustica::GpuDevice& gpuDevice = *context.gpuDevice;

    sceneEditor.setGpuDevice(gpuDevice);
    sceneEditor.initStreamlineAndWindow();
    if (context.application)
        sceneEditor.setApplication(context.application);

    SceneEditor* sceneEditorPtr = &sceneEditor;
    rendering->initializeRenderer(gpuDevice,
        std::make_shared<caustica::render::RenderSceneTypeFactory>(),
        caustica::EngineSceneCallbacks{
            .OnSceneLoaded = [sceneEditorPtr]() { sceneEditorPtr->SceneLoaded(); },
            .OnSceneUnloading = [sceneEditorPtr]() { sceneEditorPtr->SceneUnloading(); },
        });

    caustica::EngineRenderer* engineRenderer = rendering->renderer();
    if (!engineRenderer)
        return;

    sceneEditor.AttachRenderResources(
        engineRenderer->shaderFactory(),
        engineRenderer->commonPasses(),
        engineRenderer->bindingCache(),
        engineRenderer->descriptorTable(),
        engineRenderer->textureLoader());
    sceneEditor.AttachSceneServices(
        *engineRenderer->sceneManager(),
        *engineRenderer->renderCore());
    sceneEditor.AttachLightingPasses(engineRenderer->lightingPasses());
    sceneEditor.AttachRayTracingResources(engineRenderer->rayTracingResources());
    sceneEditor.AttachGaussianSplatPasses(engineRenderer->gaussianSplatPasses());

    rendering->createPathTracer(caustica::PathTracerSessionParams{
        .gpuDevice = gpuDevice,
        .settings = sceneEditor.GetPathTracerSettings(),
        .runtimeState = sceneEditor.GetRenderRuntimeState(),
        .sceneTime = sceneEditor.GetSceneTimeRef(),
        .diagnostics = m_config.diagnostics,
        .frameExtensions = m_config.frameExtensions,
    });

    sceneEditor.AttachWorldRenderer(engineRenderer->worldRenderer());
    assert(engineRenderer->rayTracingResources().isAttached()
        && engineRenderer->gaussianSplatPasses().isAttached());

    sceneEditor.Init(m_config.preferredScene, engineRenderer->shaderFactory());

    if (m_config.refreshEnvMapMediaList)
    {
        engineRenderer->lightingPasses().refreshEnvironmentMapMediaList(
            GetLocalPath(c_AssetsFolder), std::filesystem::path());
    }

    if (m_config.sessionState && m_config.cmdLine && m_config.applyCmdLineToSessionState)
        caustica::render::InitializeRenderSessionStateFromCommandLine(*m_config.sessionState, *m_config.cmdLine);

    if (m_config.sessionState && m_config.postAppInit)
        LocalConfig::PostAppInit(*m_config.sessionState);
}

void EditorSceneSubsystem::onBeginFrame(caustica::GpuDevice& /*gpuDevice*/)
{
    m_config.sceneEditor.beginFrame();
}

void EditorSceneSubsystem::onUpdate(float elapsedTimeSeconds, bool windowFocused)
{
    if (windowFocused)
        m_config.sceneEditor.Animate(elapsedTimeSeconds);
}

void EditorSceneSubsystem::onRenderScene(caustica::GpuDevice& gpuDevice)
{
    if (m_config.sceneEditor.shouldSkipRender())
        return;

    m_config.sceneEditor.Render(gpuDevice.GetCurrentFramebuffer(true));
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
