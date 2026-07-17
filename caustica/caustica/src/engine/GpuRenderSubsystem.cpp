#include <cassert>
#include <cfloat>
#include <engine/GpuRenderSubsystem.h>
#include <engine/RenderInfra.h>
#include <engine/SessionCamera.h>
#include <engine/SceneSession.h>

#include <assets/AssetSystem.h>
#include <assets/loader/ShaderFactory.h>
#include <backend/GpuDevice.h>
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
#include <render/worldRenderer/PathTracingContext.h>
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

    if (!infra.initialize(gpuDevice, params.assetSystem))
        return false;

    m_accelStructs = AccelStructManager(gpuDevice.getDevice());
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
    m_gpuDevice = &params.gpuDevice;
    m_assetSystem = &params.assetSystem;
    m_settings = &params.settings;
    m_runtimeState = &params.runtimeState;
    m_diagnostics = &params.diagnostics;
    m_sceneTime = &params.sceneTime;
    m_cmdLine = params.cmdLine;

    m_pathTracingContext = std::make_unique<render::PathTracingContext>(render::PathTracingContext{
        .gpuDevice = params.gpuDevice,
        .sceneManager = *sceneSession.manager,
        .camera = m_renderCamera,
        .accelStructs = m_accelStructs,
        .settings = params.settings,
        .runtimeState = params.runtimeState,
        .scenePasses = m_scenePasses,
        .shaderFactory = infra.shaderFactory,
        .renderDevice = *infra.renderDevice,
        .bindingCache = *infra.bindingCache,
        .textureCache = infra.textureLoader,
        .descriptorTable = infra.descriptorTable,
        .sceneTime = params.sceneTime,
        .diagnostics = params.diagnostics,
    });

    m_worldRenderer = std::make_unique<render::WorldRenderer>(*m_pathTracingContext);
    m_worldRenderer->createBindingLayouts(infra.bindlessLayout);

    render::ScenePassWireParams sceneWireParams{
        .gpuDevice = params.gpuDevice,
        .sceneManager = *sceneSession.manager,
        .accelStructs = m_accelStructs,
        .worldRenderer = *m_worldRenderer,
        .settings = params.settings,
        .invalidation = params.runtimeState.Invalidation,
        .gaussianSplatsSummary = params.runtimeState.GaussianSplats,
        .lighting = m_scenePasses.lighting,
        .bindingCache = *infra.bindingCache,
        .shaderFactory = infra.shaderFactory,
        .renderDevice = *infra.renderDevice,
    };
    sceneWireParams.onGaussianSplatTemporalReset = [worldRenderer = m_worldRenderer.get()]() {
        worldRenderer->setGaussianSplatTemporalReset(true);
    };
    sceneWireParams.getRenderTargets = [worldRenderer = m_worldRenderer.get()]() {
        return worldRenderer->getRenderTargets();
    };
    sceneWireParams.getShaderDebug = [worldRenderer = m_worldRenderer.get()]() {
        return worldRenderer->getShaderDebug();
    };
    m_scenePasses.wireSession(sceneWireParams);
    return true;
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
            m_accelStructs.clearMeshAccelStructs(*scene);
        }
    }

    if (m_assetSystem)
        m_assetSystem->clearSceneAssets();

    if (m_worldRenderer)
        m_worldRenderer->onSceneUnloading();
    m_accelStructs.releaseGpuResources();
    if (m_renderInfra && m_renderInfra->bindingCache)
        m_renderInfra->bindingCache->clear();
    m_scenePasses.lighting.sceneUnloading();
    m_scenePasses.gaussianSplats.sceneUnloading();
}

void GpuRenderSubsystem::refreshEnvironmentMapMediaList(const std::filesystem::path& assetsRoot,
    const std::filesystem::path& scenePath)
{
    m_scenePasses.lighting.refreshEnvironmentMapMediaList(assetsRoot, scenePath);
}

void GpuRenderSubsystem::applySampleSettingsFromScene()
{
    ::SceneManager* manager = sceneManager();
    if (!m_settings || !manager || !m_sessionCamera)
        return;

    const auto scene = manager->getScene();
    if (!scene)
        return;

    if (std::shared_ptr<SampleSettings> sampleSettings = scene->getSampleSettingsNode())
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
    if (m_worldRenderer)
        m_worldRenderer->onSceneLoaded();

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
            m_scenePasses.lighting.notifySceneReloaded(*scene);
            registerLoadedSceneAssets();
        }
    }
}

void GpuRenderSubsystem::onSceneLoadedGpuFinish()
{
    ::SceneManager* manager = sceneManager();
    if (!manager || !m_settings || !m_runtimeState)
        return;

    const auto scene = manager->getScene();
    if (!scene)
        return;

    if (m_cmdLine)
        m_scenePasses.gaussianSplats.onSceneLoaded();

    m_scenePasses.lighting.onSceneLoaded(*scene, *m_settings);

    SceneManager::onSceneLoadedGpuPrep(*scene, m_runtimeState->Invalidation.AccelerationStructRebuildRequested);
    m_accelStructs.resetSubInstanceCount();
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

    m_worldRenderer.reset();
    m_pathTracingContext.reset();
    if (m_sceneSession)
        m_sceneSession->reset();
    m_accelStructs = AccelStructManager{};
    m_scenePasses = {};

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

std::shared_ptr<ShaderFactory> GpuRenderSubsystem::shaderFactory() const
{
    return m_renderInfra ? m_renderInfra->shaderFactory : nullptr;
}

caustica::render::RenderDevice& GpuRenderSubsystem::renderDevice()
{
    assert(m_renderInfra != nullptr);
    return m_renderInfra->device();
}

const caustica::render::RenderDevice& GpuRenderSubsystem::renderDevice() const
{
    assert(m_renderInfra != nullptr);
    return m_renderInfra->device();
}

std::shared_ptr<ShaderFactory>& GpuRenderSubsystem::shaderFactoryRef()
{
    assert(m_renderInfra != nullptr);
    return m_renderInfra->shaderFactory;
}

std::shared_ptr<TextureLoader>& GpuRenderSubsystem::textureLoaderRef()
{
    assert(m_renderInfra != nullptr);
    return m_renderInfra->textureLoader;
}

std::shared_ptr<DescriptorTableManager>& GpuRenderSubsystem::descriptorTableRef()
{
    assert(m_renderInfra != nullptr);
    return m_renderInfra->descriptorTable;
}

BindingCache* GpuRenderSubsystem::bindingCache() const
{
    return m_renderInfra ? m_renderInfra->bindingCache.get() : nullptr;
}

std::shared_ptr<DescriptorTableManager> GpuRenderSubsystem::descriptorTable() const
{
    return m_renderInfra ? m_renderInfra->descriptorTable : nullptr;
}

BindlessTable* GpuRenderSubsystem::bindlessTable() const
{
    return m_renderInfra ? m_renderInfra->bindlessTable.get() : nullptr;
}

std::shared_ptr<TextureLoader> GpuRenderSubsystem::textureLoader() const
{
    return m_renderInfra ? m_renderInfra->textureLoader : nullptr;
}

CameraController& GpuRenderSubsystem::camera()
{
    assert(m_sessionCamera != nullptr);
    return m_sessionCamera->camera;
}

const CameraController& GpuRenderSubsystem::camera() const
{
    assert(m_sessionCamera != nullptr);
    return m_sessionCamera->camera;
}

SceneManager* GpuRenderSubsystem::sceneManager() const
{
    return m_sceneSession ? m_sceneSession->manager.get() : nullptr;
}

nvrhi::BindingLayoutHandle GpuRenderSubsystem::bindlessLayout() const
{
    return m_renderInfra ? m_renderInfra->bindlessLayout : nullptr;
}

} // namespace caustica
