#include <engine/internal/GpuRenderSubsystem.h>
#include <engine/SceneGaussianSplatLogic.h>
#include <engine/GpuSharedCaches.h>
#include <engine/SceneSession.h>

#include <filesystem>

#include <assets/AssetSystem.h>
#include <backend/GpuDevice.h>
#include <core/log.h>
#include <core/path_utils.h>
#include <engine/internal/SceneApiInternal.h>
#include <render/core/BindingCache.h>
#include <render/core/SceneGpuUpdater.h>
#include <render/passes/lighting/MaterialGpuCache.h>
#include <render/WorldRenderer.h>
#include <render/SceneLightingPasses.h>
#include <scene/Scene.h>
#include <scene/SceneManager.h>
#include <scene/SceneObjects.h>
#include <scene/SceneRenderData.h>

namespace caustica
{
namespace
{

::SceneManager* sessionManager(const SceneSession* session)
{
    return session ? session->manager.get() : nullptr;
}

void applySceneLookAfterLightingReset(
    PathTracerSettings& settings,
    render::SceneLightingPasses& lighting,
    const Scene& scene)
{
    const SceneSettings* sceneSettings = scene.getSceneSettings();
    if (!sceneSettings)
        return;

    if (sceneSettings->environment)
    {
        const EnvironmentLookSettings& env = *sceneSettings->environment;
        if (env.tintColor)
            settings.EnvironmentMapParams.TintColor = *env.tintColor;
        if (env.intensity)
            settings.EnvironmentMapParams.Intensity = *env.intensity;
        if (env.rotationXYZ)
            settings.EnvironmentMapParams.RotationXYZ = *env.rotationXYZ;
        if (env.visibleToCamera)
            settings.EnvironmentMapParams.VisibleToCamera = *env.visibleToCamera;
        if (env.enabled)
            settings.EnvironmentMapParams.enabled = *env.enabled;
        if (env.overrideSource && !env.overrideSource->empty())
            lighting.setEnvMapOverrideSource(*env.overrideSource);
    }

    if (sceneSettings->gaussianSplat)
    {
        const GaussianSplatLookSettings& splat = *sceneSettings->gaussianSplat;
        if (splat.footprintScale)
            settings.GaussianSplatScale = *splat.footprintScale;
        if (splat.alphaScale)
            settings.GaussianSplatAlphaScale = *splat.alphaScale;
        if (splat.brightness)
            settings.GaussianSplatBrightness = *splat.brightness;
        if (splat.tintColor)
            settings.GaussianSplatTintColor = *splat.tintColor;
        if (splat.applyToneMapping)
            settings.GaussianSplatApplyToneMapping = *splat.applyToneMapping;
        if (splat.asEmitter)
            settings.GaussianSplatAsEmitter = *splat.asEmitter;
        if (splat.emissionIntensity)
            settings.GaussianSplatEmissionIntensity = *splat.emissionIntensity;
        if (splat.alphaCullThreshold)
            settings.GaussianSplatAlphaCullThreshold = *splat.alphaCullThreshold;
        if (splat.shadowStrength)
            settings.GaussianSplatShadowStrength = *splat.shadowStrength;
    }
}

} // namespace

GpuRenderSubsystem::GpuRenderSubsystem() = default;

GpuRenderSubsystem::~GpuRenderSubsystem()
{
    shutdown();
}

bool GpuRenderSubsystem::initialize(const gpuRenderSubsystemInitParams& params)
{
    m_gpuSharedCaches = &params.gpuSharedCaches;
    m_sceneSession = &params.sceneSession;
    m_worldRenderer = &params.worldRenderer;
    m_gpuDevice = &params.gpuDevice;
    m_assetSystem = &params.assetSystem;
    m_settings = &params.settings;
    m_runtimeState = &params.runtimeState;
    m_diagnostics = &params.diagnostics;
    m_shutdown = false;
    return m_worldRenderer != nullptr && m_sceneSession != nullptr && m_gpuSharedCaches != nullptr;
}

void GpuRenderSubsystem::onSceneUnloading()
{
    if (m_assetSystem)
        m_assetSystem->clearSceneAssets();

    if (m_worldRenderer)
    {
        m_worldRenderer->onSceneUnloading();
        m_worldRenderer->accelStructs().releaseGpuResources();
        m_worldRenderer->lightingPasses().sceneUnloading();
        m_worldRenderer->gaussianSplatPasses().sceneUnloading();
    }
    if (m_gpuSharedCaches && m_gpuSharedCaches->bindingCache)
        m_gpuSharedCaches->bindingCache->clear();
}

size_t GpuRenderSubsystem::pendingTextureFinalizeCount()
{
    if (!m_gpuSharedCaches || !m_gpuSharedCaches->textureLoader)
        return 0;
    return m_gpuSharedCaches->textureLoader->pendingFinalizeCount();
}

bool GpuRenderSubsystem::flushTextures(float timeLimitMs)
{
    if (!m_gpuSharedCaches || !m_gpuSharedCaches->textureLoader || !m_gpuSharedCaches->renderDevice || !m_assetSystem)
        return false;
    m_assetSystem->processRenderingThreadCommands(*m_gpuSharedCaches->renderDevice, timeLimitMs);
    if (timeLimitMs <= 0.f)
        m_assetSystem->loadingFinished();
    return !m_gpuSharedCaches->textureLoader->gpuFinalizeFailed()
        && m_gpuSharedCaches->renderDevice->getDevice()->isDeviceHealthy();
}

void GpuRenderSubsystem::beginSceneGpuLoad()
{
    if (m_gpuSharedCaches && m_gpuSharedCaches->textureLoader)
        m_gpuSharedCaches->textureLoader->clearGpuFinalizeFailure();
}

bool GpuRenderSubsystem::bindWorld(const scene::SceneRenderData& renderData)
{
    ::SceneManager* manager = sessionManager(m_sceneSession);
    auto scene = manager ? manager->getScene() : nullptr;
    const std::filesystem::path scenePath = manager ? manager->getCurrentScenePath() : std::filesystem::path{};
    (void)renderData;
    if (m_worldRenderer)
        m_worldRenderer->onSceneLoaded(scene, scenePath);
    return scene && m_worldRenderer && m_gpuSharedCaches && m_gpuSharedCaches->renderDevice
        && m_gpuSharedCaches->renderDevice->getDevice()->isDeviceHealthy();
}

size_t GpuRenderSubsystem::uploadMeshes(
    const scene::SceneRenderData& renderData,
    size_t meshBegin,
    size_t targetUploadBytes)
{
    if (!m_worldRenderer)
        return renderData.staticData().meshSnapshots.size();
    return render::SceneGpuUpdater::uploadMeshesAfterLoad(
        renderData,
        m_worldRenderer->sceneGpuResources(),
        m_gpuSharedCaches ? m_gpuSharedCaches->descriptorTable.get() : nullptr,
        meshBegin,
        targetUploadBytes);
}

bool GpuRenderSubsystem::finalizeBind(const scene::SceneRenderData& renderData)
{
    ::SceneManager* manager = sessionManager(m_sceneSession);
    auto scene = manager ? manager->getScene() : nullptr;
    const std::filesystem::path scenePath = manager ? manager->getCurrentScenePath() : std::filesystem::path{};
    if (!scene || !m_worldRenderer)
        return false;

    if (!render::SceneGpuUpdater::finalizeAfterLoad(
        *scene,
        renderData,
        m_worldRenderer->sceneGpuResources(),
        m_gpuSharedCaches ? m_gpuSharedCaches->descriptorTable.get() : nullptr,
        0))
        return false;
    m_worldRenderer->lightingPasses().notifySceneReloaded(renderData.staticData().geometryCount);

    if (m_gpuSharedCaches && m_gpuSharedCaches->renderDevice)
    {
        if (auto materials = m_worldRenderer->lightingPasses().materials())
        {
            materials->reloadMaterialsForSceneSwitch(
                *m_gpuSharedCaches->renderDevice,
                renderData.staticData().materialSnapshots,
                scenePath,
                mediaRootForScene(scenePath));
        }
    }
    return m_gpuSharedCaches && m_gpuSharedCaches->renderDevice
        && m_gpuSharedCaches->renderDevice->getDevice()->isDeviceHealthy();
}

bool GpuRenderSubsystem::finishLoadedScene(const scene::SceneRenderData& renderData)
{
    ::SceneManager* manager = sessionManager(m_sceneSession);
    if (!manager || !m_settings || !m_runtimeState || !m_worldRenderer)
        return false;

    const auto scene = manager->getScene();
    if (!scene)
        return false;

    SceneGaussianSplatLogic::onSceneLoaded(
        m_worldRenderer->gaussianSplatPasses(), *m_settings);
    m_worldRenderer->lightingPasses().onSceneLoaded(renderData, *m_settings);
    // Lighting reset wipes EnvironmentMapParams / env override. Re-apply only
    // fields that were actually present in the scene JSON.
    applySceneLookAfterLightingReset(*m_settings, m_worldRenderer->lightingPasses(), *scene);
    // Animations / prep only — AS comes from StructureGpu AccelOnly (ADR 0001 P3),
    // not the sync-frame AccelerationStructRebuildRequested path.
    bool unusedAccelFlag = false;
    SceneManager::onSceneLoadedGpuPrep(*scene, unusedAccelFlag);
    m_worldRenderer->accelStructs().resetSubInstanceCount();
    // onSceneUnloading clears the live RT pipeline bindings; the retained cache
    // must rebind them before MainPathTrace can dispatch the new scene.
    m_runtimeState->Invalidation.ShaderReloadRequested = true;
    m_settings->MaterialVariantIndex = 0;
    scene->requestGpuStructureSync(StructureGpuUploadMode::AccelOnly);
    // Open-scene "still loading" is LoadSession::FirstPresent (wait for StructureGpu commit).
    // OMM / opacity queue writes AppDiagnostics::asyncLoadingInProgress as RT scratch,
    // mirrored into LoadSession::secondaryStreaming on Logic.
    (void)m_diagnostics;
    return m_gpuDevice && m_gpuDevice->getDevice()
        && m_gpuDevice->getDevice()->isDeviceHealthy();
}

void GpuRenderSubsystem::shutdown()
{
    if (m_shutdown)
        return;

    m_shutdown = true;

    // Drain GPU work, then release Streamline/DLSS/DLSS-G and scene GPU resources
    // before destroying WorldRenderer. Skipping this leaves live SL resources for
    // slShutdown and can hang or crash on window close.
    if (m_gpuDevice)
    {
        // THREADING: sync-point, shutdown — ADR 0002 allowed.
        m_gpuDevice->waitForRenderThreadIdle();
        if (caustica::rhi::Device* device = m_gpuDevice->getDevice())
            device->waitForIdle();
    }

    if (::SceneManager* manager = sessionManager(m_sceneSession))
    {
        if (const std::shared_ptr<Scene> scene = manager->getScene())
            scene->prepareForUnload();
    }

    if (m_worldRenderer)
        m_worldRenderer->releaseStreamlineTemporalResources();
    onSceneUnloading();

    if (m_gpuDevice)
    {
        if (caustica::rhi::Device* device = m_gpuDevice->getDevice())
        {
            // THREADING: sync-point, shutdown — ADR 0002 allowed.
            device->waitForIdle();
            device->runGarbageCollection();
        }
    }

    if (m_worldRenderer)
    {
        m_worldRenderer->destroy();
        m_worldRenderer = nullptr;
    }
    if (m_sceneSession)
        m_sceneSession->reset();

    m_gpuDevice = nullptr;
    m_assetSystem = nullptr; // borrowed; AssetPlugin owns AssetSystem::shutdown()
    m_settings = nullptr;
    m_runtimeState = nullptr;
    m_diagnostics = nullptr;
    m_sceneSession = nullptr;

    if (m_gpuSharedCaches)
    {
        m_gpuSharedCaches->shutdown();
        m_gpuSharedCaches = nullptr;
    }
}

} // namespace caustica
