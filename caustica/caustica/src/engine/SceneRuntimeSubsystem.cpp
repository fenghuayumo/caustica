#include <engine/SceneRuntimeSubsystem.h>
#include <scene/Scene.h>

#include <engine/GpuRenderSubsystem.h>
#include <engine/SceneRuntime.h>
#include <engine/SceneRuntimeRegistration.h>
#include <engine/SubsystemCollection.h>

#include <core/path_utils.h>
#include <render/core/RenderSceneTypeFactory.h>
#include <render/RenderSessionState.h>
#include <render/SceneLightingPasses.h>
#include <render/worldRenderer/WorldRenderer.h>
#include <render/pipeline/RenderPipelineRegistry.h>

namespace caustica
{

SceneRuntimeSubsystem::SceneRuntimeSubsystem(SceneRuntimeSubsystemConfig config)
    : m_config(std::move(config))
{
}

void SceneRuntimeSubsystem::initialize(EngineInitContext& context)
{
    if (!context.gpuDevice || !context.subsystems)
        return;

    auto* gpuRenderSubsystem = context.subsystems->get<GpuRenderSubsystem>();
    if (!gpuRenderSubsystem)
        return;

    SceneRuntime& sceneRuntime = m_config.sceneRuntime;
    GpuDevice& gpuDevice = *context.gpuDevice;

    sceneRuntime.setGpuDevice(gpuDevice);
    sceneRuntime.initStreamlineAndWindow();
    if (context.application)
        sceneRuntime.setApplication(context.application);

    SceneRuntime* sceneRuntimePtr = &sceneRuntime;
    gpuRenderSubsystem->initializeSession(GpuRenderSubsystemInitParams{
        .gpuDevice = gpuDevice,
        .settings = sceneRuntime.GetPathTracerSettings(),
        .runtimeState = sceneRuntime.GetRenderRuntimeState(),
        .sceneTime = sceneRuntime.GetSceneTimeRef(),
        .diagnostics = m_config.diagnostics,
        .cmdLine = m_config.cmdLine,
        .sceneTypeFactory = std::make_shared<render::RenderSceneTypeFactory>(),
        .sceneCallbacks = EngineSceneCallbacks{
            .OnSceneLoaded = [sceneRuntimePtr]() { sceneRuntimePtr->SceneLoaded(); },
            .OnSceneUnloading = [sceneRuntimePtr]() { sceneRuntimePtr->SceneUnloading(); },
        },
    });

    sceneRuntime.bindGpuRenderSubsystem(*gpuRenderSubsystem);
    sceneRuntime.Init(m_config.preferredScene, gpuRenderSubsystem->shaderFactory());

    if (m_config.refreshEnvMapMediaList)
    {
        gpuRenderSubsystem->lightingPasses().refreshEnvironmentMapMediaList(
            GetLocalPath(c_AssetsFolder), std::filesystem::path());
    }

    if (m_config.sessionState && m_config.cmdLine && m_config.applyCmdLineToSessionState)
        render::InitializeRenderSessionStateFromCommandLine(*m_config.sessionState, *m_config.cmdLine);

    if (auto* worldRenderer = sceneRuntime.GetWorldRenderer())
        sceneRuntime.registerRenderPipelinePlugins(worldRenderer->pipelineRegistry());

    onInitializePost(context);
}

void SceneRuntimeSubsystem::onBeginFrame(GpuDevice& /*gpuDevice*/)
{
    onBeforeBeginFrame();
    m_config.sceneRuntime.beginFrame();
}

void SceneRuntimeSubsystem::onUpdate(float elapsedTimeSeconds, bool windowFocused)
{
    if (windowFocused)
        m_config.sceneRuntime.Animate(elapsedTimeSeconds);
}

void SceneRuntimeSubsystem::onPrepareRenderScene(GpuDevice& /*gpuDevice*/)
{
    if (m_config.sceneRuntime.shouldSkipRender())
        return;

    m_config.sceneRuntime.prepareRenderFrame();
}

void SceneRuntimeSubsystem::onRenderScene(GpuDevice& gpuDevice)
{
    SceneRuntime& sceneRuntime = m_config.sceneRuntime;
    if (sceneRuntime.shouldSkipRender())
        return;

    auto* worldRenderer = sceneRuntime.GetWorldRenderer();
    if (!worldRenderer)
        return;

    worldRenderer->render(gpuDevice.GetCurrentFramebuffer(true));
    sceneRuntime.afterWorldRender(gpuDevice);
    sceneRuntime.recordFrameTiming(gpuDevice);
}

void SceneRuntimeSubsystem::onBackBufferResizing()
{
    m_config.sceneRuntime.BackBufferResizing();
}

bool SceneRuntimeSubsystem::skipRenderPhase() const
{
    return m_config.sceneRuntime.shouldSkipRender();
}

bool SceneRuntimeSubsystem::shouldRenderWhenUnfocused() const
{
    return m_config.sceneRuntime.ShouldRenderUnfocused();
}

void SceneRuntimeSubsystem::onInitializePost(EngineInitContext& /*context*/)
{
}

void SceneRuntimeSubsystem::onBeforeBeginFrame()
{
}

} // namespace caustica
