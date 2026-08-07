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
#include <scene/Scene.h>
#include <scene/SceneManager.h>
#include <scene/SceneRenderData.h>

namespace caustica
{
namespace
{

::SceneManager* sessionManager(const SceneSession* session)
{
    return session ? session->manager.get() : nullptr;
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

void GpuRenderSubsystem::flushTextures(float timeLimitMs)
{
    if (!m_gpuSharedCaches || !m_gpuSharedCaches->textureLoader || !m_gpuSharedCaches->renderDevice || !m_assetSystem)
        return;
    m_assetSystem->processRenderingThreadCommands(*m_gpuSharedCaches->renderDevice, timeLimitMs);
    if (timeLimitMs <= 0.f)
        m_assetSystem->loadingFinished();
}

void GpuRenderSubsystem::bindWorld(const scene::SceneRenderData& renderData)
{
    ::SceneManager* manager = sessionManager(m_sceneSession);
    auto scene = manager ? manager->getScene() : nullptr;
    const std::filesystem::path scenePath = manager ? manager->getCurrentScenePath() : std::filesystem::path{};
    (void)renderData;
    if (m_worldRenderer)
        m_worldRenderer->onSceneLoaded(scene, scenePath);
}

size_t GpuRenderSubsystem::uploadMeshes(
    const scene::SceneRenderData& renderData,
    size_t meshBegin,
    size_t maxMeshes)
{
    if (!m_worldRenderer)
        return renderData.meshSnapshots.size();
    return render::SceneGpuUpdater::uploadMeshesAfterLoad(
        renderData,
        m_worldRenderer->sceneGpuResources(),
        m_gpuSharedCaches ? m_gpuSharedCaches->descriptorTable.get() : nullptr,
        meshBegin,
        maxMeshes);
}

void GpuRenderSubsystem::finalizeBind(const scene::SceneRenderData& renderData)
{
    ::SceneManager* manager = sessionManager(m_sceneSession);
    auto scene = manager ? manager->getScene() : nullptr;
    const std::filesystem::path scenePath = manager ? manager->getCurrentScenePath() : std::filesystem::path{};
    if (!scene || !m_worldRenderer)
        return;

    render::SceneGpuUpdater::finalizeAfterLoad(
        *scene,
        renderData,
        m_worldRenderer->sceneGpuResources(),
        m_gpuSharedCaches ? m_gpuSharedCaches->descriptorTable.get() : nullptr,
        0);
    m_worldRenderer->lightingPasses().notifySceneReloaded(renderData.geometryCount);

    if (m_gpuSharedCaches && m_gpuSharedCaches->renderDevice)
    {
        if (auto materials = m_worldRenderer->lightingPasses().materials())
        {
            materials->reloadMaterialsForSceneSwitch(
                *m_gpuSharedCaches->renderDevice,
                renderData.materialSnapshots,
                scenePath,
                getLocalPath(c_AssetsFolder));
        }
    }
}

void GpuRenderSubsystem::finishLoadedScene(const scene::SceneRenderData& renderData)
{
    ::SceneManager* manager = sessionManager(m_sceneSession);
    if (!manager || !m_settings || !m_runtimeState || !m_worldRenderer)
        return;

    const auto scene = manager->getScene();
    if (!scene)
        return;

    SceneGaussianSplatLogic::onSceneLoaded(m_worldRenderer->gaussianSplatPasses());
    m_worldRenderer->lightingPasses().onSceneLoaded(renderData, *m_settings);
    // Animations / prep only — AS comes from StructureGpu AccelOnly (ADR 0001 P3),
    // not the sync-frame AccelerationStructRebuildRequested path.
    bool unusedAccelFlag = false;
    SceneManager::onSceneLoadedGpuPrep(*scene, unusedAccelFlag);
    m_worldRenderer->accelStructs().resetSubInstanceCount();
    // onSceneUnloading clears m_ptPipeline*; without this the RT cache stays
    // "ready" and never rebinds, so MainPathTrace dispatches a null pipeline.
    m_runtimeState->Invalidation.ShaderReloadRequested = true;
    m_settings->MaterialVariantIndex = 0;
    scene->requestGpuStructureSync(StructureGpuUploadMode::AccelOnly);
    if (m_diagnostics)
        m_diagnostics->asyncLoadingInProgress = true;
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
