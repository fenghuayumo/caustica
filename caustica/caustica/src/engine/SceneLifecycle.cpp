#include <engine/App.h>
#include <engine/internal/GpuRenderSubsystem.h>
#include <engine/GpuSharedCaches.h>
#include <engine/AppResources.h>
#include <engine/internal/WorldRendererAccess.h>
#include <engine/SceneViewState.h>
#include <cassert>
#include <engine/SceneLifecycle.h>
#include <engine/internal/ActiveSceneAccess.h>
#include <engine/SceneQuery.h>
#include <engine/CameraApi.h>
#include <engine/RenderSessionApi.h>
#include <engine/EnqueueRenderCommand.h>
#include <engine/internal/SceneApiInternal.h>
#include <engine/RenderThread.h>
#include <engine/ActiveScene.h>
#include <assets/AssetSystem.h>
#include <backend/GpuDevice.h>
#include <core/command_line.h>
#include <core/log.h>
#include <core/path_utils.h>
#include <core/vfs/VFS.h>
#include <cstdarg>
#include <scene/Scene.h>
#include <scene/SceneManager.h>
#include <scene/scene_utils.h>
#include <scene/SceneCameraAccess.h>
#include <scene/SceneEcs.h>
#include <scene/SceneRenderExtract.h>
#include <scene/SceneTypes.h>
#include <render/core/PathTracerSettings.h>
#include <render/WorldRenderer.h>
#include <render/core/CameraController.h>
#include <render/SceneLightingPasses.h>
#include <assets/loader/TextureLoader.h>
#include <algorithm>
#include <cctype>
#include <cfloat>
#include <thread>
#include <chrono>


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
            viewState.progressLoading.start("Starting up...");
            viewState.progressLoading.Set(50);
        }
    }

    void syncCameraFromScene(App& app)
    {
        auto scenePtr = caustica::activeScene(app);
        CameraController* cam = cameraController(app);
        if (!scenePtr || !cam)
            return;

        const auto* ew = scenePtr->getEntityWorld();
        bool syncedCamera = false;
        if (ew)
        {
            // The published render snapshot is still empty during onSceneLoaded.
            // Read the logic-owned registration order while synchronizing the controller.
            const auto& cameraEntities = ew->cameraEntitiesInRegistrationOrder();
            const uint32_t selectedIndex = cam->selectedCameraIndex();
            if (!cameraEntities.empty())
            {
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
        }
        if (!syncedCamera)
            cam->setupDefaultCamera();
    }
}

using namespace caustica::render;

namespace caustica
{

void bindCameraControllerSideEffects(App& app)
{
    PathTracerSettings* cfg = settings(app);
    CameraController* cam = cameraController(app);
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
        || !cameraController(app) || !wrResource)
    {
        caustica::fatal("caustica::initializeScene requires GpuSharedCaches / CameraController / WorldRenderer wiring");
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
    if (device && device->getDevice()->queryFeatureSupport(caustica::rhi::Feature::RayTracingOpacityMicromap))
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

    caustica::info("initializeScene: loading '%s'", sceneArg.c_str());
    setCurrentScene(app, sceneArg);
    // Do not block Startup on CPU import + GPU bind. The render thread and frame
    // loop are not running yet; sync-waiting here freezes the window and piles
    // upload work onto the wrong thread. SceneAnimate::updateLoading finishes
    // the load after App::run starts (same path as Open Scene).
    caustica::info("initializeScene: async load started for '%s'", sceneArg.c_str());
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
    vs->sceneTime = 0.0;
    vs->uncompressedTextures.clear();
    vs->gpuBind = {};

    // Contract: stop submit → RT idle → (RT) waitForIdle → Streamline → tear down.
    vs->sceneGpuSuspended.store(true, std::memory_order_release);
    if (vs->progressLoading.Active())
        vs->progressLoading.Set(15);
    detail::sceneSwitchTrace("onSceneUnloading: draining GPU");

    // Scene::prepareForUnload mutates live ECS/resource ownership. Drain render
    // work first, then perform that mutation in the logic domain.
    app.waitForRenderThreadIdle();
    if (::SceneManager* manager = detail::sessionManager(app))
    {
        if (const std::shared_ptr<Scene> scene = manager->getScene())
            scene->prepareForUnload();
    }

    detail::sceneSwitchTrace("onSceneUnloading: GPU teardown on render thread");
    EnqueueRenderCommandAndWait(app, [&app]() {
        GpuDevice* device = gpuDevice(app);
        caustica::rhi::Device* rhi = device ? device->getDevice() : nullptr;
        // THREADING: sync-point, RT-only — never waitForIdle from the logic thread.
        if (rhi && !rhi->waitForIdle())
        {
            if (device)
                device->setShuttingDown(true);
            return;
        }

        if (render::WorldRenderer* wr = worldRenderer(app))
            wr->releaseStreamlineTemporalResources();

        if (GpuRenderSubsystem* gr = app.tryResource<GpuRenderSubsystem>())
            gr->onSceneUnloading();

        if (rhi)
        {
            rhi->waitForIdle();
            rhi->runGarbageCollection();
        }
    });

    if (vs->progressLoading.Active())
        vs->progressLoading.Set(30);
    detail::sceneSwitchTrace("onSceneUnloading: GPU drain complete");

    clearActiveScene(app);
}

namespace
{

void applySampleSettingsFromScene(App& app, ::SceneManager& manager)
{
    PathTracerSettings* cfg = settings(app);
    CameraController* cam = cameraController(app);
    if (!cfg || !cam)
        return;

    const auto scene = manager.getScene();
    if (!scene)
        return;

    if (const SampleSettings* sampleSettings = scene->getSampleSettings())
    {
        cfg->RealtimeMode = sampleSettings->realtimeMode.value_or(cfg->RealtimeMode);
        cfg->EnableAnimations = sampleSettings->enableAnimations.value_or(cfg->EnableAnimations);
        cfg->EnableKeyframes = sampleSettings->enableKeyframes.value_or(cfg->EnableKeyframes);
        if (sampleSettings->startingCamera.has_value())
            cam->setSelectedCameraIndex(sampleSettings->startingCamera.value() + 1);
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
    cfg->EnableKeyframes = false;
    cfg->RealtimeMode = false;

    applySampleSettingsFromScene(app, manager);

    if (cmd.stopAnimations)
    {
        cfg->EnableAnimations = false;
        cfg->EnableKeyframes = false;
    }
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

    const std::filesystem::path scenePath = currentScenePath(app);
    const std::string sceneName = currentSceneName(app).empty()
        ? scenePath.filename().generic_string()
        : currentSceneName(app);

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

void abortGpuBind(SceneViewState& vs)
{
    vs.gpuBind = {};
    vs.sceneGpuSuspended.store(false, std::memory_order_release);
    if (vs.progressLoading.Active())
        vs.progressLoading.stop();
}

} // namespace

void onSceneLoaded(App& app)
{
    SceneViewState* vs = viewState(app);
    GpuRenderSubsystem* gr = app.tryResource<GpuRenderSubsystem>();
    ::SceneManager* manager = detail::sessionManager(app);
    const CommandLineOptions* cmd = cmdLine(app);
    PathTracerSettings* cfg = settings(app);
    if (!manager || !vs || !cmd || !cfg || !gr)
    {
        if (vs)
            vs->sceneGpuSuspended.store(false, std::memory_order_release);
        return;
    }

    const std::filesystem::path assetsRoot = getLocalPath(c_AssetsFolder);
    if (render::WorldRenderer* wrResource = worldRenderer(app))
        wrResource->lightingPasses().refreshEnvironmentMapMediaList(assetsRoot, currentScenePath(app));

    vs->progressLoading.Set(50);
    detail::sceneSwitchTrace("onSceneLoaded: logic setup");
    applyLogicThreadSceneLoadSetup(app, *manager, *cmd);

    const scene::SceneRenderData* renderData = nullptr;
    if (const std::shared_ptr<Scene> scenePtr = activeScene(app))
    {
        if (GpuDevice* device = gpuDevice(app))
            renderData = &scenePtr->extractAndPublishForGpuSetup(device->getFrameIndex());
    }
    if (!renderData)
    {
        detail::sceneSwitchTrace("onSceneLoaded: no renderData");
        abortGpuBind(*vs);
        return;
    }

    registerLoadedSceneAssets(app, *manager);
    syncCameraFromScene(app);

    // Start multi-frame bind; sceneGpuSuspended stays true until LogicFinish.
    const size_t texturesPending = gr->pendingTextureFinalizeCount();
    vs->gpuBind = {
        .phase = texturesPending > 0
            ? SceneViewState::GpuBindPhase::Textures
            : SceneViewState::GpuBindPhase::World,
        .renderData = renderData,
        .texturesTotal = texturesPending,
        .meshBegin = 0,
        .meshTotal = renderData->meshSnapshots.size(),
    };
    vs->progressLoading.Set(55);
    detail::sceneSwitchTrace("onSceneLoaded: bind job textures=%zu meshes=%zu",
        texturesPending, vs->gpuBind.meshTotal);
}

void tickSceneGpuBind(App& app)
{
    SceneViewState* vs = viewState(app);
    if (!vs || vs->gpuBind.phase == SceneViewState::GpuBindPhase::None)
        return;

    GpuRenderSubsystem* gr = app.tryResource<GpuRenderSubsystem>();
    const scene::SceneRenderData* data = vs->gpuBind.renderData;
    if (!gr || !data)
    {
        abortGpuBind(*vs);
        return;
    }

    using Phase = SceneViewState::GpuBindPhase;
    auto& job = vs->gpuBind;

    switch (job.phase)
    {
    case Phase::Textures:
    {
        EnqueueRenderCommandAndWait(app, [gr]() { gr->flushTextures(8.f); });
        const size_t remaining = gr->pendingTextureFinalizeCount();
        if (job.texturesTotal > 0)
        {
            const float done = float(job.texturesTotal - remaining) / float(job.texturesTotal);
            vs->progressLoading.Set(55 + int(done * 15.f));
        }
        if (remaining == 0)
        {
            EnqueueRenderCommandAndWait(app, [gr]() { gr->flushTextures(0.f); });
            job.phase = Phase::World;
        }
        break;
    }
    case Phase::World:
        EnqueueRenderCommandAndWait(app, [gr, data]() { gr->bindWorld(*data); });
        job.phase = Phase::Meshes;
        vs->progressLoading.Set(72);
        break;
    case Phase::Meshes:
    {
        const size_t start = job.meshBegin;
        size_t next = start;
        EnqueueRenderCommandAndWait(app, [gr, data, start, &next]() {
            next = gr->uploadMeshes(*data, start, 1);
        });
        job.meshBegin = next;
        if (job.meshTotal > 0)
            vs->progressLoading.Set(72 + int(float(next) / float(job.meshTotal) * 18.f));
        if (next <= start && next < job.meshTotal)
        {
            detail::sceneSwitchTrace("tickSceneGpuBind: mesh stall at %zu", start);
            abortGpuBind(*vs);
            break;
        }
        if (next >= job.meshTotal)
            job.phase = Phase::Finalize;
        break;
    }
    case Phase::Finalize:
        EnqueueRenderCommandAndWait(app, [gr, data]() { gr->finalizeBind(*data); });
        vs->progressLoading.Set(93);
        job.phase = Phase::LogicFinish;
        break;
    case Phase::LogicFinish:
    {
        PathTracerSettings* cfg = settings(app);
        const CommandLineOptions* cmd = cmdLine(app);
        gr->finishLoadedScene(*data);
        collectUncompressedTextures(app);
        if (cfg && cmd)
        {
            applyCmdLinePostLoadOverrides(*cfg, *cmd);
            if (!cmd->cameraPosDirUp.empty())
                setCurrentCameraPosDirUp(app, cmd->cameraPosDirUp);
        }
        if (CameraController* cam = cameraController(app))
            cam->syncPreviousViewFromCurrent();
        vs->progressLoading.Set(100);
        vs->progressLoading.stop();
        vs->gpuBind = {};
        vs->sceneGpuSuspended.store(false, std::memory_order_release);
        detail::sceneSwitchTrace("onSceneLoaded: complete");
        break;
    }
    case Phase::None:
    default:
        break;
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
        caustica::rhi::TextureDesc desc = texture->gpu.texture->getDesc();
        if (caustica::rhi::getFormatInfo(desc.format).blockSize != 1)
            return;
        TextureCompressionType compressionType = normalMap ? (TextureCompressionType::Normalmap) : (
            (caustica::rhi::getFormatInfo(desc.format).isSRGB) ? (TextureCompressionType::GenericSRGB) : (TextureCompressionType::GenericLinear));

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
    if (const SceneViewState* vs = viewState(app);
        vs && vs->gpuBind.phase != SceneViewState::GpuBindPhase::None)
        return true;

    AppDiagnostics* diag = diagnostics(app);
    RenderRuntimeState* runtime = runtimeState(app);
    if (!diag || !runtime)
        return false;
    return diag->asyncLoadingInProgress
        || runtime->Invalidation.ShaderAndACRefreshDelayedRequest > 0;
}

void sceneSwitchTrace(const char* fmt, ...)
{
    char buf[1024];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    detail::sceneSwitchTrace("%s", buf);
}

} // namespace caustica
