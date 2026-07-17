#include <engine/GpuRenderSubsystem.h>
#include <engine/SceneGaussianSplatLogic.h>
#include <engine/GpuSharedCaches.h>
#include <engine/SceneSession.h>

#include <filesystem>

#include <assets/AssetSystem.h>
#include <backend/GpuDevice.h>
#include <render/core/BindingCache.h>
#include <render/core/SceneGpuUpdater.h>
#include <render/worldRenderer/WorldRenderer.h>
#include <scene/Scene.h>
#include <scene/SceneManager.h>

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

void GpuRenderSubsystem::onSceneLoadedGpuPrep(const scene::SceneRenderData& renderData)
{
    ::SceneManager* manager = sessionManager(m_sceneSession);
    auto scene = manager ? manager->getScene() : nullptr;
    const std::filesystem::path scenePath = manager ? manager->getCurrentScenePath() : std::filesystem::path{};

    if (m_worldRenderer)
        m_worldRenderer->onSceneLoaded(scene, scenePath);

    if (m_gpuSharedCaches && m_gpuSharedCaches->textureLoader && m_gpuSharedCaches->renderDevice && m_assetSystem)
    {
        m_assetSystem->processRenderingThreadCommands(*m_gpuSharedCaches->renderDevice, 0.f);
        m_assetSystem->loadingFinished();
    }

    if (scene)
    {
        render::SceneGpuUpdater::refreshAfterLoad(
            *scene,
            renderData,
            m_gpuSharedCaches ? m_gpuSharedCaches->descriptorTable.get() : nullptr,
            0);
        if (m_worldRenderer)
            m_worldRenderer->lightingPasses().notifySceneReloaded(renderData.geometryCount);
    }
}

void GpuRenderSubsystem::onSceneLoadedGpuFinish(const scene::SceneRenderData& renderData)
{
    ::SceneManager* manager = sessionManager(m_sceneSession);
    if (!manager || !m_settings || !m_runtimeState || !m_worldRenderer)
        return;

    const auto scene = manager->getScene();
    if (!scene)
        return;

    SceneGaussianSplatLogic::onSceneLoaded(m_worldRenderer->gaussianSplatPasses());
    m_worldRenderer->lightingPasses().onSceneLoaded(renderData, *m_settings);

    SceneManager::onSceneLoadedGpuPrep(*scene, m_runtimeState->Invalidation.AccelerationStructRebuildRequested);
    m_worldRenderer->accelStructs().resetSubInstanceCount();
    m_runtimeState->Invalidation.ShaderReloadRequested = true;

    m_settings->MaterialVariantIndex = 0;

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
        if (nvrhi::IDevice* device = m_gpuDevice->getDevice())
            device->waitForIdle();
    }

    if (::SceneManager* manager = sessionManager(m_sceneSession))
    {
        if (const std::shared_ptr<Scene> scene = manager->getScene())
            scene->prepareForUnload();
    }

    onSceneUnloading();

    if (m_gpuDevice)
    {
        if (nvrhi::IDevice* device = m_gpuDevice->getDevice())
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
