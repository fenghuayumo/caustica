#include <engine/App.h>
#include <engine/GpuRenderSubsystem.h>
#include <engine/GpuSharedCaches.h>
#include <engine/AppResources.h>
#include <engine/SessionCamera.h>
#include <engine/SceneViewState.h>
#include <cassert>
#include <engine/SceneLifecycle.h>
#include <engine/SceneQuery.h>
#include <engine/CameraApi.h>
#include <engine/RenderSessionApi.h>
#include <engine/SceneApiInternal.h>
#include <engine/RenderThread.h>
#include <engine/SceneAccess.h>
#include <assets/AssetSystem.h>
#include <backend/GpuDevice.h>
#include <core/command_line.h>
#include <core/log.h>
#include <core/path_utils.h>
#include <core/vfs/VFS.h>
#include <scene/Scene.h>
#include <scene/SceneManager.h>
#include <scene/scene_utils.h>
#include <scene/SceneCameraAccess.h>
#include <scene/SceneEcs.h>
#include <scene/SceneRenderExtract.h>
#include <scene/SceneTypes.h>
#include <render/core/PathTracerSettings.h>
#include <render/worldRenderer/WorldRenderer.h>
#include <render/core/CameraController.h>
#include <render/SceneLightingPasses.h>
#include <assets/loader/TextureLoader.h>
#include <algorithm>
#include <cctype>
#include <cfloat>


namespace
{
    bool LooksLikeInlineSceneJson(const std::string& scene)
    {
        auto it = std::find_if_not(scene.begin(), scene.end(), [](unsigned char ch) {
            return std::isspace(ch);
        });
        return it != scene.end() && *it == '{';
    }

    void initViewState(SceneViewState& viewState)
    {
        if (!viewState.progressLoading.Active())
        {
            viewState.progressLoading.start("Initializing...");
            viewState.progressLoading.Set(50);
        }
    }

    void syncCameraFromScene(App& app)
    {
        auto scenePtr = caustica::activeScene(app);
        CameraController* cam = detail::sessionCamera(app);
        if (!scenePtr || !cam)
            return;

        const auto& cameraEntities = scenePtr->getCameraEntities();
        const auto* ew = scenePtr->getEntityWorld();
        bool syncedCamera = false;
        if (!cameraEntities.empty() && ew)
        {
            const uint32_t selectedIndex = cam->selectedCameraIndex();
            const uint32_t camIdx = (selectedIndex > 0) ? (selectedIndex - 1)
                : static_cast<uint32_t>(cameraEntities.size() - 1);
            if (camIdx < cameraEntities.size())
            {
                ecs::Entity camEntity = cameraEntities[camIdx];
                const auto* camComp = scene::tryGetCamera(ew->world(), camEntity);
                const auto* globalComp = ew->world().get<scene::GlobalTransformComponent>(camEntity);
                if (camComp && globalComp)
                {
                    const scene::CameraRenderProxy proxy =
                        scene::makeCameraRenderProxy(camEntity, *camComp, *globalComp);
                    PathTracerSettings* settingsPtr = caustica::settings(app);
                    scene::applyCameraRenderProxyToController(proxy, *cam, settingsPtr);
                    if (settingsPtr)
                        settingsPtr->ResetAccumulation = true;
                    if (auto* wr = caustica::worldRenderer(app))
                        wr->setGaussianSplatTemporalReset(true);
                    syncedCamera = proxy.projection == scene::CameraProjectionKind::Perspective;
                }
            }
        }
        if (!syncedCamera)
            cam->setupDefaultCamera();
    }
}

using namespace caustica::render;

namespace caustica
{

void bindSessionCameraSideEffects(App& app)
{
    PathTracerSettings* cfg = settings(app);
    CameraController* cam = detail::sessionCamera(app);
    assert(cfg);
    assert(cam);
    cam->bindSideEffects(*cfg, worldRenderer(app));
}

void initStreamlineAndWindow(App& app)
{
#if CAUSTICA_WITH_STREAMLINE
    PathTracerSettings* cfg = settings(app);
    GpuDevice* device = gpuDevice(app);
    if (!cfg || !device || device->isHeadless())
        return;

    cfg->IsDLSSSuported = device->getStreamline().isDLSSAvailable();
    cfg->IsDLSSFGSupported = device->getStreamline().isDLSSGAvailable();
    cfg->IsReflexSupported = device->getStreamline().isReflexAvailable();
    cfg->IsDLSSRRSupported = device->getStreamline().isDLSSRRAvailable();
#endif
}

void initializeScene(App& app, const std::string& preferredScene)
{
    if (SceneViewState* vs = viewState(app))
        initViewState(*vs);

    auto* wr = worldRenderer(app);
    if (!wr)
    {
        caustica::fatal("caustica::initializeScene requires a path tracer world renderer");
        return;
    }

    GpuSharedCaches* caches = gpuSharedCaches(app);
    render::WorldRenderer* wrResource = worldRenderer(app);
    if (!caches || !caches->shaderFactory || !caches->descriptorTable || !caches->textureLoader
        || !detail::sessionCamera(app) || !wrResource)
    {
        caustica::fatal("caustica::initializeScene requires GpuSharedCaches / SessionCamera / WorldRenderer wiring");
        return;
    }
    const auto shaderFactory = caches->shaderFactory;
    const auto descriptorTable = caches->descriptorTable;
    const auto textureLoader = caches->textureLoader;

    if (!wr->getRenderTargets())
        wr->createDeviceResources();

    ::SceneManager* manager = detail::sessionManager(app);
    if (!manager)
    {
        caustica::fatal("caustica::initializeScene requires scene manager");
        return;
    }

    PathTracerSettings* cfg = settings(app);
    assert(cfg);

    cfg->EnableGaussianSplats = true;

    GpuDevice* device = gpuDevice(app);
    if (device && device->getDevice()->queryFeatureSupport(nvrhi::Feature::RayTracingOpacityMicromap))
    {
        wrResource->lightingPasses().createOpacityMapsIfSupported(
            device->getDevice(), descriptorTable, textureLoader, shaderFactory);
    }

    manager->discoverAvailableScenes(getLocalPath(c_AssetsFolder));

    std::string sceneArg;
    if (LooksLikeInlineSceneJson(preferredScene))
    {
        sceneArg = preferredScene;
    }
    else
    {
        std::filesystem::path preferredScenePath(preferredScene);
        sceneArg = (!preferredScene.empty() && (preferredScenePath.is_absolute() || std::filesystem::exists(preferredScenePath)))
            ? preferredScene
            : findPreferredScene(manager->getAvailableScenes(), preferredScene);
    }

    setCurrentScene(app, sceneArg);
}

void setCurrentScene(App& app, const std::string& sceneName, bool forceReload)
{
    if (caustica::isRenderThread())
    {
        SceneViewState* vs = viewState(app);
        assert(vs);
        std::lock_guard lock(vs->pendingSceneSwitchMutex);
        vs->pendingSceneSwitch = SceneViewState::PendingSceneSwitch{ sceneName, forceReload };
        return;
    }

    detail::applySceneSwitch(app, sceneName, forceReload);
}

void onSceneUnloading(App& app)
{
    PathTracerSettings* cfg = settings(app);
    SceneViewState* vs = viewState(app);
    assert(cfg && vs);

    cfg->EnvironmentMapParams = EnvironmentMapRuntimeParameters();
    vs->uncompressedTextures.clear();

    runGpuWorkOnRenderThread(app, [&app]() {
        if (GpuRenderSubsystem* gr = app.tryResource<GpuRenderSubsystem>())
            gr->onSceneUnloading();
    });

    if (auto* access = app.tryResource<SceneAccess>())
        access->active.reset();
}

namespace
{

void applySampleSettingsFromScene(App& app, ::SceneManager& manager)
{
    PathTracerSettings* cfg = settings(app);
    SessionCamera* sessionCam = sessionCameraResource(app);
    if (!cfg || !sessionCam)
        return;

    const auto scene = manager.getScene();
    if (!scene)
        return;

    if (const SampleSettings* sampleSettings = scene->getSampleSettings())
    {
        cfg->RealtimeMode = sampleSettings->realtimeMode.value_or(cfg->RealtimeMode);
        cfg->EnableAnimations = sampleSettings->enableAnimations.value_or(cfg->EnableAnimations);
        if (sampleSettings->startingCamera.has_value())
            sessionCam->camera.setSelectedCameraIndex(sampleSettings->startingCamera.value() + 1);
        if (sampleSettings->realtimeFireflyFilter.has_value())
        {
            cfg->RealtimeFireflyFilterThreshold = sampleSettings->realtimeFireflyFilter.value();
            cfg->RealtimeFireflyFilterEnabled = true;
        }
        cfg->BounceCount = sampleSettings->maxBounces.value_or(cfg->BounceCount);
        cfg->DiffuseBounceCount = sampleSettings->maxDiffuseBounces.value_or(cfg->DiffuseBounceCount);
        cfg->TexLODBias = sampleSettings->textureMIPBias.value_or(cfg->TexLODBias);
    }
}

void applyLogicThreadSceneLoadSetup(App& app, ::SceneManager& manager, const CommandLineOptions& cmd)
{
    PathTracerSettings* cfg = settings(app);
    SceneViewState* vs = viewState(app);
    if (!cfg || !vs)
        return;

    vs->sceneTime = 0.0;
    cfg->EnableAnimations = false;
    cfg->RealtimeMode = false;

    applySampleSettingsFromScene(app, manager);

    if (cmd.stopAnimations)
        cfg->EnableAnimations = false;
    if (cmd.OverrideToRealtimeMode)
        cfg->RealtimeMode = true;
    if (cmd.OverrideToReferenceMode)
        cfg->RealtimeMode = false;

    cfg->ToneMappingParams.exposureCompensation = 2.0f;
    cfg->ToneMappingParams.exposureValue = 0.0f;

    // Logic-thread hierarchy snapshot before RT exclusive GPU upload.
    if (auto scene = manager.getScene())
    {
        if (auto* entityWorld = scene->getEntityWorld())
        {
            entityWorld->refreshHierarchy(scene::PreviousTransformPolicy::CaptureCurrent);
            entityWorld->syncPreviousTransformsFromCurrent();
        }
    }
}

void applyCmdLinePostLoadOverrides(PathTracerSettings& cfg, const CommandLineOptions& cmd)
{
    if (cmd.OverrideAutoexposureOff)
    {
        cfg.ToneMappingParams.autoExposure = false;
        cfg.ToneMappingParams.exposureValue = 0.0f;
    }
    if (cmd.OverrideExposureOffset != FLT_MAX)
        cfg.ToneMappingParams.exposureCompensation = cmd.OverrideExposureOffset;
    if (cmd.DisableFireflyFilters)
    {
        cfg.RealtimeFireflyFilterEnabled = false;
        cfg.ReferenceFireflyFilterEnabled = false;
    }
    if (cmd.DisablePostProcessFilters)
        cfg.EnableBloom = false;
}

void registerLoadedSceneAssets(App& app, ::SceneManager& manager)
{
    AssetSystem* assets = app.tryResource<AssetSystem>();
    const auto scene = manager.getScene();
    if (!assets || !scene)
        return;

    assets->clearSceneAssets();

    const std::filesystem::path scenePath = manager.getCurrentScenePath();
    const std::string sceneName = manager.getCurrentSceneName().empty()
        ? scenePath.filename().generic_string()
        : manager.getCurrentSceneName();

    Handle<SceneAsset> sceneAsset = assets->registerSceneAsset(scene, scenePath, sceneName);
    if (!sceneAsset)
        return;
    scene->setAssetHandle(sceneAsset);

    for (const std::shared_ptr<MeshInfo>& mesh : scene->getMeshes())
    {
        Handle<MeshAsset> meshAsset = assets->registerMeshAsset(mesh, scenePath, mesh ? mesh->name : std::string());
        if (mesh)
            mesh->asset = meshAsset;
        if (meshAsset)
            assets->addDependency(sceneAsset.id(), meshAsset.id());
    }

    for (const std::shared_ptr<Material>& material : scene->getMaterials())
    {
        Handle<MaterialAsset> materialAsset = assets->registerMaterialAsset(
            material,
            scenePath,
            material ? material->name : std::string());
        if (!materialAsset)
            continue;
        material->asset = materialAsset;
        assets->addDependency(sceneAsset.id(), materialAsset.id());

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
                assets->addDependency(materialAsset.id(), texture.id());
        }
    }
}

} // namespace

void onSceneLoaded(App& app)
{
    GpuRenderSubsystem* gr = app.tryResource<GpuRenderSubsystem>();
    if (!gr)
        return;

    ::SceneManager* manager = detail::sessionManager(app);
    SceneViewState* vs = viewState(app);
    const CommandLineOptions* cmd = cmdLine(app);
    PathTracerSettings* cfg = settings(app);
    if (!manager || !vs || !cmd || !cfg)
        return;

    syncSceneAccess(app);

    const std::filesystem::path assetsRoot = getLocalPath(c_AssetsFolder);
    if (render::WorldRenderer* wrResource = worldRenderer(app))
        wrResource->lightingPasses().refreshEnvironmentMapMediaList(assetsRoot, manager->getCurrentScenePath());

    vs->progressLoading.Set(50);
    vs->progressLoading.Set(55);

    applyLogicThreadSceneLoadSetup(app, *manager, *cmd);

    runGpuWorkOnRenderThread(app, [&app]() {
        if (GpuRenderSubsystem* gpu = app.tryResource<GpuRenderSubsystem>())
            gpu->onSceneLoadedGpuPrep();
    });

    registerLoadedSceneAssets(app, *manager);

    syncCameraFromScene(app);

    vs->progressLoading.Set(60);

    gr->onSceneLoadedGpuFinish();

    collectUncompressedTextures(app);

    vs->progressLoading.Set(70);
    vs->progressLoading.Set(90);

    applyCmdLinePostLoadOverrides(*cfg, *cmd);
    if (!cmd->cameraPosDirUp.empty())
        setCurrentCameraPosDirUp(app, cmd->cameraPosDirUp);

    if (CameraController* cam = detail::sessionCamera(app))
        cam->syncPreviousViewFromCurrent();

    vs->progressLoading.Set(100);

    if (GpuDevice* device = gpuDevice(app))
    {
        device->setPreparedRenderFrameIndex(device->getFrameIndex());
        if (const std::shared_ptr<Scene> scenePtr = activeScene(app))
            scenePtr->extractAndPublishRenderSnapshot(device->getPreparedRenderFrameIndex());
    }
}

void collectUncompressedTextures(App& app)
{
    SceneViewState* vs = viewState(app);
    assert(vs);
    vs->uncompressedTextures.clear();

    auto listUncompressedTextureIfNeeded = [&](Handle<ImageAsset> texture, bool normalMap)
    {
        if (texture == nullptr || texture->gpu.texture == nullptr)
            return;
        nvrhi::TextureDesc desc = texture->gpu.texture->getDesc();
        if (nvrhi::getFormatInfo(desc.format).blockSize != 1)
            return;
        TextureCompressionType compressionType = normalMap ? (TextureCompressionType::Normalmap) : (
            (nvrhi::getFormatInfo(desc.format).isSRGB) ? (TextureCompressionType::GenericSRGB) : (TextureCompressionType::GenericLinear));

        auto it = vs->uncompressedTextures.insert(std::make_pair(texture, compressionType));
        if (!it.second)
        {
            assert(it.first->second == compressionType);
            return;
        }
    };
    if (render::WorldRenderer* wrResource = worldRenderer(app))
    {
        wrResource->lightingPasses().forEachUsedMaterialTexture([&](Handle<ImageAsset> texture, bool normalMap)
        {
            listUncompressedTextureIfNeeded(texture, normalMap);
        });
    }
}

bool hasAsyncLoadingInProgress(const App& app)
{
    AppDiagnostics* diag = diagnostics(app);
    RenderRuntimeState* runtime = runtimeState(app);
    if (!diag || !runtime)
        return false;
    return diag->asyncLoadingInProgress
        || runtime->Invalidation.ShaderAndACRefreshDelayedRequest > 0;
}

} // namespace caustica
