#include <cassert>
#include <cfloat>
#include <engine/GpuRenderSubsystem.h>
#include <engine/PathTracingRuntime.h>
#include <engine/RenderInfra.h>
#include <engine/SessionCamera.h>
#include <engine/SceneSession.h>

#include <assets/AssetSystem.h>
#include <assets/loader/ShaderFactory.h>
#include <backend/GpuDevice.h>
#include <backend/IDescriptorTableManager.h>
#include <core/command_line.h>
#include <core/log.h>
#include <core/path_utils.h>
#include <render/core/BindingCache.h>
#include <render/core/BindlessTable.h>
#include <render/core/DescriptorTableManager.h>
#include <render/core/SceneGpuUpdater.h>
#include <render/core/RenderDevice.h>
#include <render/PathTracerScenePasses.h>
#include <render/worldRenderer/WorldRenderer.h>
#include <scene/Scene.h>
#include <scene/SceneEcs.h>
#include <scene/SceneManager.h>
#include <scene/SceneTypes.h>

namespace caustica
{

GpuRenderSubsystem::GpuRenderSubsystem() = default;

GpuRenderSubsystem::~GpuRenderSubsystem()
{
    shutdown();
}

bool GpuRenderSubsystem::initialize(const GpuRenderSubsystemInitParams& params)
{
    GpuDevice& gpuDevice = params.gpuDevice;
    RenderInfra& infra = params.renderInfra;
    SessionCamera& sessionCamera = params.sessionCamera;
    SceneSession& sceneSession = params.sceneSession;
    PathTracingRuntime& pathTracing = params.pathTracingRuntime;

    if (!infra.initialize(gpuDevice, params.assetSystem))
        return false;

    sessionCamera.camera.camera().setRotateSpeed(.003f);

    std::shared_ptr<IDescriptorTableManager> descriptorTable = infra.descriptorTable;
    if (!sceneSession.create(
            gpuDevice,
            *infra.shaderFactory,
            infra.textureLoader,
            descriptorTable,
            params.sceneTypeFactory,
            std::move(params.sceneCallbacks.OnSceneLoaded),
            std::move(params.sceneCallbacks.OnSceneUnloading)))
    {
        return false;
    }

    m_renderInfra = &infra;
    m_sessionCamera = &sessionCamera;
    m_sceneSession = &sceneSession;
    m_pathTracing = &pathTracing;
    m_gpuDevice = &params.gpuDevice;
    m_assetSystem = &params.assetSystem;
    m_settings = &params.settings;
    m_runtimeState = &params.runtimeState;
    m_diagnostics = &params.diagnostics;
    m_sceneTime = &params.sceneTime;
    m_cmdLine = params.cmdLine;

    return pathTracing.create(PathTracingRuntime::CreateParams{
        .gpuDevice = gpuDevice,
        .renderInfra = infra,
        .sceneManager = *sceneSession.manager,
        .settings = params.settings,
        .runtimeState = params.runtimeState,
        .diagnostics = params.diagnostics,
        .sceneTime = params.sceneTime,
    });
}

void GpuRenderSubsystem::onSceneUnloading()
{
    // Break asset shared_ptr cycles and drop extract retained refs BEFORE clearing
    // the AssetSystem store / destroying the scene. Otherwise MeshInfo↔MeshAsset
    // cycles keep BLAS/buffers alive past GpuDevice::shutdown and heap-corrupt on close.
    if (::SceneManager* manager = sceneManager())
    {
        if (const std::shared_ptr<Scene> scene = manager->getScene())
        {
            scene->prepareForUnload();
            if (m_pathTracing)
                m_pathTracing->accelStructs().clearMeshAccelStructs(*scene);
        }
    }

    if (m_assetSystem)
        m_assetSystem->clearSceneAssets();

    if (m_pathTracing)
    {
        if (render::WorldRenderer* wr = m_pathTracing->worldRenderer())
            wr->onSceneUnloading();
        m_pathTracing->accelStructs().releaseGpuResources();
        m_pathTracing->lightingPasses().sceneUnloading();
        m_pathTracing->gaussianSplatPasses().sceneUnloading();
    }
    if (m_renderInfra && m_renderInfra->bindingCache)
        m_renderInfra->bindingCache->clear();
}

void GpuRenderSubsystem::applySampleSettingsFromScene()
{
    ::SceneManager* manager = sceneManager();
    if (!m_settings || !manager || !m_sessionCamera)
        return;

    const auto scene = manager->getScene();
    if (!scene)
        return;

    if (const SampleSettings* sampleSettings = scene->getSampleSettings())
    {
        m_settings->RealtimeMode = sampleSettings->realtimeMode.value_or(m_settings->RealtimeMode);
        m_settings->EnableAnimations = sampleSettings->enableAnimations.value_or(m_settings->EnableAnimations);
        if (sampleSettings->startingCamera.has_value())
            m_sessionCamera->camera.setSelectedCameraIndex(sampleSettings->startingCamera.value() + 1);
        if (sampleSettings->realtimeFireflyFilter.has_value())
        {
            m_settings->RealtimeFireflyFilterThreshold = sampleSettings->realtimeFireflyFilter.value();
            m_settings->RealtimeFireflyFilterEnabled = true;
        }
        m_settings->BounceCount = sampleSettings->maxBounces.value_or(m_settings->BounceCount);
        m_settings->DiffuseBounceCount = sampleSettings->maxDiffuseBounces.value_or(m_settings->DiffuseBounceCount);
        m_settings->TexLODBias = sampleSettings->textureMIPBias.value_or(m_settings->TexLODBias);
    }
}

void GpuRenderSubsystem::onSceneLoadedBegin()
{
    if (!m_settings || !m_sceneTime)
        return;

    *m_sceneTime = 0.0;
    m_settings->EnableAnimations = false;
    m_settings->RealtimeMode = false;

    applySampleSettingsFromScene();

    if (m_cmdLine && m_cmdLine->stopAnimations)
        m_settings->EnableAnimations = false;

    if (m_cmdLine)
    {
        if (m_cmdLine->OverrideToRealtimeMode)
            m_settings->RealtimeMode = true;
        if (m_cmdLine->OverrideToReferenceMode)
            m_settings->RealtimeMode = false;
    }

    m_settings->ToneMappingParams.exposureCompensation = 2.0f;
    m_settings->ToneMappingParams.exposureValue = 0.0f;

    // Logic-thread hierarchy snapshot before RT exclusive GPU upload.
    if (::SceneManager* manager = sceneManager())
    {
        if (auto scene = manager->getScene())
        {
            if (auto* entityWorld = scene->getEntityWorld())
            {
                entityWorld->refreshHierarchy(scene::PreviousTransformPolicy::CaptureCurrent);
                entityWorld->syncPreviousTransformsFromCurrent();
            }
        }
    }
}

void GpuRenderSubsystem::onSceneLoadedGpuPrep()
{
    if (m_pathTracing)
    {
        if (render::WorldRenderer* wr = m_pathTracing->worldRenderer())
            wr->onSceneLoaded();
    }

    if (m_renderInfra && m_renderInfra->textureLoader && m_renderInfra->renderDevice && m_assetSystem)
    {
        m_assetSystem->processRenderingThreadCommands(*m_renderInfra->renderDevice, 0.f);
        m_assetSystem->loadingFinished();
    }

    if (::SceneManager* manager = sceneManager())
    {
        if (auto scene = manager->getScene())
        {
            render::SceneGpuUpdater::refreshAfterLoad(*scene, 0);
            if (m_pathTracing)
                m_pathTracing->lightingPasses().notifySceneReloaded(*scene);
            registerLoadedSceneAssets();
        }
    }
}

void GpuRenderSubsystem::onSceneLoadedGpuFinish()
{
    ::SceneManager* manager = sceneManager();
    if (!manager || !m_settings || !m_runtimeState || !m_pathTracing)
        return;

    const auto scene = manager->getScene();
    if (!scene)
        return;

    if (m_cmdLine)
        m_pathTracing->gaussianSplatPasses().onSceneLoaded();

    m_pathTracing->lightingPasses().onSceneLoaded(*scene, *m_settings);

    SceneManager::onSceneLoadedGpuPrep(*scene, m_runtimeState->Invalidation.AccelerationStructRebuildRequested);
    m_pathTracing->accelStructs().resetSubInstanceCount();
    m_runtimeState->Invalidation.ShaderReloadRequested = true;

    m_settings->MaterialVariantIndex = 0;

    if (m_diagnostics)
        m_diagnostics->asyncLoadingInProgress = true;
}

void GpuRenderSubsystem::setSceneLoadingCallbacks(std::function<void()> onLoaded, std::function<void()> onUnloading)
{
    if (::SceneManager* manager = sceneManager())
        manager->setLoadingCallbacks(std::move(onLoaded), std::move(onUnloading));
}

void GpuRenderSubsystem::applyCmdLinePostLoadOverrides()
{
    if (!m_settings || !m_cmdLine)
        return;

    if (m_cmdLine->OverrideAutoexposureOff)
    {
        m_settings->ToneMappingParams.autoExposure = false;
        m_settings->ToneMappingParams.exposureValue = 0.0f;
    }
    if (m_cmdLine->OverrideExposureOffset != FLT_MAX)
        m_settings->ToneMappingParams.exposureCompensation = m_cmdLine->OverrideExposureOffset;
    if (m_cmdLine->DisableFireflyFilters)
    {
        m_settings->RealtimeFireflyFilterEnabled = false;
        m_settings->ReferenceFireflyFilterEnabled = false;
    }
    if (m_cmdLine->DisablePostProcessFilters)
        m_settings->EnableBloom = false;
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

    onSceneUnloading();

    if (m_gpuDevice)
    {
        if (nvrhi::IDevice* device = m_gpuDevice->getDevice())
        {
            device->waitForIdle();
            device->runGarbageCollection();
        }
    }

    if (m_pathTracing)
    {
        m_pathTracing->destroy();
        m_pathTracing = nullptr;
    }
    if (m_sceneSession)
        m_sceneSession->reset();

    m_gpuDevice = nullptr;
    AssetSystem* assetSystem = m_assetSystem;
    m_assetSystem = nullptr;
    m_settings = nullptr;
    m_runtimeState = nullptr;
    m_diagnostics = nullptr;
    m_sceneTime = nullptr;
    m_cmdLine = nullptr;
    m_sessionCamera = nullptr;
    m_sceneSession = nullptr;

    if (m_renderInfra)
    {
        m_renderInfra->shutdown();
        m_renderInfra = nullptr;
    }

    if (assetSystem)
        assetSystem->shutdown();
}

void GpuRenderSubsystem::registerLoadedSceneAssets()
{
    ::SceneManager* manager = sceneManager();
    if (!m_assetSystem || !manager)
        return;

    const std::shared_ptr<Scene> scene = manager->getScene();
    if (!scene)
        return;

    m_assetSystem->clearSceneAssets();

    const std::filesystem::path scenePath = manager->getCurrentScenePath();
    const std::string sceneName = manager->getCurrentSceneName().empty()
        ? scenePath.filename().generic_string()
        : manager->getCurrentSceneName();

    Handle<SceneAsset> sceneAsset = m_assetSystem->registerSceneAsset(scene, scenePath, sceneName);
    if (!sceneAsset)
        return;
    scene->setAssetHandle(sceneAsset);

    for (const std::shared_ptr<MeshInfo>& mesh : scene->getMeshes())
    {
        Handle<MeshAsset> meshAsset = m_assetSystem->registerMeshAsset(mesh, scenePath, mesh ? mesh->name : std::string());
        if (mesh)
            mesh->asset = meshAsset;
        if (meshAsset)
            m_assetSystem->addDependency(sceneAsset.id(), meshAsset.id());
    }

    for (const std::shared_ptr<Material>& material : scene->getMaterials())
    {
        Handle<MaterialAsset> materialAsset = m_assetSystem->registerMaterialAsset(
            material,
            scenePath,
            material ? material->name : std::string());
        if (!materialAsset)
            continue;
        material->asset = materialAsset;

        m_assetSystem->addDependency(sceneAsset.id(), materialAsset.id());

        const Handle<ImageAsset> textures[] = {
            material->baseOrDiffuseTexture,
            material->metalRoughOrSpecularTexture,
            material->normalTexture,
            material->emissiveTexture,
            material->occlusionTexture,
            material->transmissionTexture,
            material->opacityTexture,
        };

        for (const Handle<ImageAsset>& texture : textures)
        {
            if (texture)
                m_assetSystem->addDependency(materialAsset.id(), texture.id());
        }
    }
}

SceneManager* GpuRenderSubsystem::sceneManager() const
{
    return m_sceneSession ? m_sceneSession->manager.get() : nullptr;
}

} // namespace caustica
