#include <engine/App.h>
#include <engine/internal/GpuRenderSubsystem.h>
#include <engine/GpuSharedCaches.h>
#include <engine/AppResources.h>
#include <engine/internal/WorldRendererAccess.h>
#include <engine/LoadSession.h>
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
#include <core/task/TaskRuntime.h>
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
#include <memory>
#include <utility>
#include <functional>
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
    vs->loadSession.streamStep = LoadStreamStep::Textures;
    vs->loadSession.renderData = nullptr;
    vs->loadSession.stepInFlight = false;
    vs->loadSession.teardownGpuDone.store(false, std::memory_order_release);
    vs->loadSession.phase = LoadSessionPhase::Teardown;
    task::bumpLoadGeneration();

    // Exclusive teardown window — cleared by tickLoadSession when RT finishes (no AndWait).
    vs->sceneGpuSuspended.store(true, std::memory_order_release);
    if (vs->progressLoading.Active())
        vs->progressLoading.Set(vs->loadSession.progressPercent());
    detail::sceneSwitchTrace("onSceneUnloading: draining render queue");

    // Scene::prepareForUnload mutates live ECS/resource ownership. Drain scheduled
    // render work first, then mutate on Logic; GPU resource release is async on RT.
    app.waitForRenderThreadIdle();
    if (::SceneManager* manager = detail::sessionManager(app))
    {
        if (const std::shared_ptr<Scene> scene = manager->getScene())
            scene->prepareForUnload();
    }

    detail::sceneSwitchTrace("onSceneUnloading: enqueue GPU teardown");
    EnqueueRenderCommand(app, [&app, vs]() {
        GpuDevice* device = gpuDevice(app);
        caustica::rhi::Device* rhi = device ? device->getDevice() : nullptr;
        // THREADING: sync-point, RT-only — never waitForIdle from the logic thread.
        if (rhi && !rhi->waitForIdle())
        {
            if (device)
                device->setShuttingDown(true);
            vs->loadSession.teardownGpuDone.store(true, std::memory_order_release);
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
        vs->loadSession.teardownGpuDone.store(true, std::memory_order_release);
    });

    // Drop logic-side active scene immediately; GPU teardown runs under suspension.
    clearActiveScene(app);
    detail::sceneSwitchTrace("onSceneUnloading: waiting for RT teardown via LoadSession::Teardown");
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

void abortLoadSession(SceneViewState& vs)
{
    vs.loadSession.reset();
    vs.sceneGpuSuspended.store(false, std::memory_order_release);
    task::bumpLoadGeneration();
    if (vs.progressLoading.Active())
        vs.progressLoading.stop();
}

void syncLoadProgress(SceneViewState& vs)
{
    if (!vs.loadSession.isActive())
        return;
    if (!vs.progressLoading.Active())
        vs.progressLoading.start("Loading scene...");
    vs.progressLoading.Set(vs.loadSession.progressPercent());
}

// Complete one in-flight Render step; returns true if a step was pending (caller should
// not enqueue another this tick).
bool pollLoadStreamStep(LoadSession& session)
{
    if (!session.stepInFlight)
        return false;
    const uint8_t status = session.stepStatus.load(std::memory_order_acquire);
    if (status == 0)
        return true; // still running on RT
    session.stepInFlight = false;
    return false;
}

struct LoadStreamStepJob
{
    std::function<void()> body;
    LoadSession* session = nullptr;
    uint64_t generation = 0;

    static void run(void* user)
    {
        std::unique_ptr<LoadStreamStepJob> job(static_cast<LoadStreamStepJob*>(user));
        if (job->session && job->generation == task::loadGeneration() && job->body)
            job->body();
        // Always publish completion so Logic can advance / abort after LoadGen bump.
        if (job->session)
        {
            uint8_t expected = 0;
            job->session->stepStatus.compare_exchange_strong(
                expected, 1, std::memory_order_release, std::memory_order_relaxed);
        }
    }
};

void beginLoadStreamStep(App& /*app*/, LoadSession& session, std::function<void()> body)
{
    session.stepStatus.store(0, std::memory_order_release);
    session.stepInFlight = true;

    auto job = std::make_unique<LoadStreamStepJob>();
    job->body = std::move(body);
    job->session = &session;
    job->generation = task::loadGeneration();
    (void)task::launch(
        "LoadSession.StreamStep",
        task::Priority::High,
        task::Affinity::Render,
        &LoadStreamStepJob::run,
        job.release(),
        task::loadSessionPipe());
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
            abortLoadSession(*vs);
        return;
    }

    const std::filesystem::path assetsRoot = getLocalPath(c_AssetsFolder);
    if (render::WorldRenderer* wrResource = worldRenderer(app))
        wrResource->lightingPasses().refreshEnvironmentMapMediaList(assetsRoot, currentScenePath(app));

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
        abortLoadSession(*vs);
        return;
    }

    registerLoadedSceneAssets(app, *manager);
    syncCameraFromScene(app);

    // GpuStreaming with present enabled (suspension already cleared after teardown).
    const size_t texturesPending = gr->pendingTextureFinalizeCount();
    vs->loadSession.reset();
    vs->loadSession.phase = LoadSessionPhase::GpuStreaming;
    vs->loadSession.streamStep = texturesPending > 0
        ? LoadStreamStep::Textures
        : LoadStreamStep::World;
    vs->loadSession.renderData = renderData;
    vs->loadSession.texturesTotal = texturesPending;
    vs->loadSession.stepTexturesRemaining.store(texturesPending, std::memory_order_relaxed);
    vs->loadSession.meshBegin = 0;
    vs->loadSession.meshTotal = renderData->meshSnapshots.size();
    vs->sceneGpuSuspended.store(false, std::memory_order_release);
    syncLoadProgress(*vs);
    detail::sceneSwitchTrace("onSceneLoaded: LoadSession GpuStreaming textures=%zu meshes=%zu",
        texturesPending, vs->loadSession.meshTotal);
}

void tickLoadSession(App& app)
{
    SceneViewState* vs = viewState(app);
    if (!vs)
        return;

    auto& session = vs->loadSession;

    // Mirror RT OMM / opacity streaming into LoadSession (sole busy signal).
    if (AppDiagnostics* diag = diagnostics(app))
    {
        session.secondaryStreaming.store(diag->asyncLoadingInProgress, std::memory_order_relaxed);
        // Prefer session.secondaryStreaming; keep diag as RT scratch only.
    }

    if (!session.isActive())
        return;

    syncLoadProgress(*vs);

    if (session.phase == LoadSessionPhase::Teardown)
    {
        if (!session.teardownGpuDone.load(std::memory_order_acquire))
            return;

        vs->sceneGpuSuspended.store(false, std::memory_order_release);
        session.phase = LoadSessionPhase::Importing;
        session.teardownGpuDone.store(false, std::memory_order_relaxed);
        syncLoadProgress(*vs);
        detail::sceneSwitchTrace("LoadSession: Teardown complete → Importing");

        if (session.deferredImportPending)
        {
            session.deferredImportPending = false;
            if (::SceneManager* manager = detail::sessionManager(app))
            {
                detail::sceneSwitchTrace("LoadSession: starting deferred CPU import");
                manager->setAsyncLoadingEnabled(true);
                manager->beginLoadingScene(
                    std::make_shared<caustica::NativeFileSystem>(),
                    manager->getCurrentScenePath());
                if (!manager->isSceneLoading() && manager->getScene() == nullptr)
                {
                    caustica::error("Unable to load deferred scene");
                    manager->clearScene();
                    clearActiveScene(app);
                    abortLoadSession(*vs);
                }
            }
        }
        return;
    }

    if (session.phase == LoadSessionPhase::Importing)
        return; // CPU worker; onSceneLoaded advances to GpuStreaming

    if (session.phase == LoadSessionPhase::Ready)
    {
        session.reset();
        if (vs->progressLoading.Active())
            vs->progressLoading.stop();
        detail::sceneSwitchTrace("LoadSession: Ready");
        return;
    }

    if (session.phase == LoadSessionPhase::Finalizing)
    {
        GpuRenderSubsystem* gr = app.tryResource<GpuRenderSubsystem>();
        const scene::SceneRenderData* data = session.renderData;
        if (!gr || !data)
        {
            abortLoadSession(*vs);
            return;
        }

        PathTracerSettings* cfg = settings(app);
        const CommandLineOptions* cmd = cmdLine(app);
        // LoadSession owns orchestration; GpuRenderSubsystem is the RT/logic step executor.
        gr->finishLoadedScene(*data); // requests StructureGpu AccelOnly
        collectUncompressedTextures(app);
        if (cfg && cmd)
        {
            applyCmdLinePostLoadOverrides(*cfg, *cmd);
            if (!cmd->cameraPosDirUp.empty())
                setCurrentCameraPosDirUp(app, cmd->cameraPosDirUp);
        }
        if (CameraController* cam = cameraController(app))
            cam->syncPreviousViewFromCurrent();

        session.phase = LoadSessionPhase::FirstPresent;
        syncLoadProgress(*vs);
        detail::sceneSwitchTrace("LoadSession: Finalizing → FirstPresent (wait StructureGpu)");
        return;
    }

    if (session.phase == LoadSessionPhase::FirstPresent)
    {
        // Hold progress until StructureGpu AccelOnly finishes (committed serve ready).
        const std::shared_ptr<Scene> scene = activeScene(app);
        if (!scene)
        {
            abortLoadSession(*vs);
            return;
        }
        if (scene->needsGpuStructureSync() || scene->structureGpuBuildInFlight())
        {
            syncLoadProgress(*vs);
            return;
        }
        if (!scene->committedRenderData())
        {
            // Extract may still freeze/enqueue this frame; wait one more tick.
            syncLoadProgress(*vs);
            return;
        }

        session.phase = LoadSessionPhase::Ready;
        syncLoadProgress(*vs);
        return;
    }

    if (session.phase != LoadSessionPhase::GpuStreaming)
        return;

    GpuRenderSubsystem* gr = app.tryResource<GpuRenderSubsystem>();
    const scene::SceneRenderData* data = session.renderData;
    if (!gr || !data)
    {
        abortLoadSession(*vs);
        return;
    }

    // Still waiting on RT — present keeps running on the render domain.
    if (session.stepInFlight)
    {
        if (pollLoadStreamStep(session))
            return;

        if (session.stepStatus.load(std::memory_order_relaxed) == 2)
        {
            detail::sceneSwitchTrace("tickLoadSession: stream step failed");
            abortLoadSession(*vs);
            return;
        }

        // Advance stream state from the completed step, then fall through to enqueue next.
        switch (session.streamStep)
        {
        case LoadStreamStep::Textures:
            if (session.textureDrainPending)
            {
                session.textureDrainPending = false;
                session.streamStep = LoadStreamStep::World;
            }
            // else budgeted flush; stay on Textures (maybe remaining==0 → drain next)
            break;
        case LoadStreamStep::World:
            session.streamStep = LoadStreamStep::Meshes;
            session.stepMeshNext.store(session.meshBegin, std::memory_order_relaxed);
            break;
        case LoadStreamStep::Meshes:
        {
            const size_t next = session.stepMeshNext.load(std::memory_order_relaxed);
            session.meshBegin = next;
            if (session.meshBegin >= session.meshTotal)
                session.streamStep = LoadStreamStep::Finalize;
            break;
        }
        case LoadStreamStep::Finalize:
            session.phase = LoadSessionPhase::Finalizing;
            syncLoadProgress(*vs);
            return;
        }
    }

    switch (session.streamStep)
    {
    case LoadStreamStep::Textures:
    {
        const size_t remaining = session.stepTexturesRemaining.load(std::memory_order_relaxed);
        if (remaining == 0)
        {
            // Final drain (loadingFinished) before World.
            session.textureDrainPending = true;
            beginLoadStreamStep(app, session, [gr, &session]() {
                gr->flushTextures(0.f);
                session.stepTexturesRemaining.store(
                    gr->pendingTextureFinalizeCount(), std::memory_order_relaxed);
                session.stepStatus.store(1, std::memory_order_release);
            });
        }
        else
        {
            beginLoadStreamStep(app, session, [gr, &session]() {
                gr->flushTextures(LoadSession::kTextureBudgetMs);
                session.stepTexturesRemaining.store(
                    gr->pendingTextureFinalizeCount(), std::memory_order_relaxed);
                session.stepStatus.store(1, std::memory_order_release);
            });
        }
        break;
    }
    case LoadStreamStep::World:
        beginLoadStreamStep(app, session, [gr, data, &session]() {
            gr->bindWorld(*data);
            session.stepStatus.store(1, std::memory_order_release);
        });
        break;
    case LoadStreamStep::Meshes:
    {
        if (session.meshBegin >= session.meshTotal)
        {
            session.streamStep = LoadStreamStep::Finalize;
            // Enqueue finalize this same tick.
            beginLoadStreamStep(app, session, [gr, data, &session]() {
                gr->finalizeBind(*data);
                session.stepStatus.store(1, std::memory_order_release);
            });
            break;
        }
        const size_t start = session.meshBegin;
        beginLoadStreamStep(app, session, [gr, data, start, &session]() {
            const size_t next = gr->uploadMeshes(*data, start, LoadSession::kMeshesPerStep);
            session.stepMeshNext.store(next, std::memory_order_relaxed);
            if (next <= start && next < data->meshSnapshots.size())
                session.stepStatus.store(2, std::memory_order_release);
            else
                session.stepStatus.store(1, std::memory_order_release);
        });
        break;
    }
    case LoadStreamStep::Finalize:
        beginLoadStreamStep(app, session, [gr, data, &session]() {
            gr->finalizeBind(*data);
            session.stepStatus.store(1, std::memory_order_release);
        });
        break;
    }

    syncLoadProgress(*vs);
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
    if (const SceneViewState* vs = viewState(app); vs && vs->loadSession.isBusy())
        return true;

    RenderRuntimeState* runtime = runtimeState(app);
    if (!runtime)
        return false;
    return runtime->Invalidation.ShaderAndACRefreshDelayedRequest > 0;
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
