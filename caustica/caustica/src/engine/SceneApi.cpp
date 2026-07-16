#include <engine/SceneApi.h>
#include <engine/ScenePlugins.h>

#include <engine/App.h>
#include <engine/GpuRenderSubsystem.h>
#include <engine/RenderThread.h>
#include <engine/SceneAccess.h>
#include <engine/SceneViewState.h>
#include <render/passes/lighting/MaterialGpuCache.h>

#include <assets/AssetSystem.h>
#include <render/RenderAppState.h>
#include <render/SceneGaussianSplatPasses.h>
#include <render/SceneLightingPasses.h>
#include <render/SceneRayTracingResources.h>
#include <render/worldRenderer/WorldRenderer.h>
#include <shaders/PathTracer/PathTracerDebug.hlsli>
#include <render/core/RenderTargets.h>
#include <render/passes/postProcess/ToneMappingPasses.h>

#include <core/path_utils.h>
#include <core/format.h>
#include <scene/scene_utils.h>
#include <scene/SceneAnimationAccess.h>
#include <scene/SceneCameraAccess.h>
#include <scene/SceneEcs.h>
#include <scene/SceneManager.h>
#include <assets/loader/ShaderFactory.h>
#include <assets/loader/TextureLoader.h>
#include <render/core/BindingCache.h>
#include <render/core/View.h>
#include <render/core/SceneGpuUpdater.h>
#include <render/core/SceneMeshEditing.h>
#include <render/core/RenderSceneTypeFactory.h>
#include <render/core/PathTracingShaderCompiler.h>
#include <assets/loader/ShaderMacro.h>
#include <scene/SceneApply.h>
#include <scene/SceneTypes.h>
#include <scene/loader/RuntimeMeshLoader.h>
#include <assets/RuntimeMeshLoadTypes.h>
#include <backend/GpuDevice.h>
#include <cassert>
#include <core/Timer.h>
#include <render/core/RenderDevice.h>
#include <core/vfs/VFS.h>
#include <ecs/Entity.h>
#include <math/math.h>
#include <shaders/light_cb.h>

#include <algorithm>
#include <cctype>
#include <cmath>

using namespace caustica::math;
using namespace caustica;
using namespace caustica::render;

const char* g_windowTitle = "caustica";
FPSLimiter g_FPSLimiter;

namespace
{
    bool LooksLikeInlineSceneJson(const std::string& scene)
    {
        auto it = std::find_if_not(scene.begin(), scene.end(), [](unsigned char ch) {
            return std::isspace(ch);
        });
        return it != scene.end() && *it == '{';
    }

    ::SceneManager* localSceneManager(App& app)
    {
        if (GpuRenderSubsystem* gpuRender = app.tryResource<GpuRenderSubsystem>())
            return gpuRender->sceneManager();
        return nullptr;
    }

    render::WorldRenderer* localWorldRenderer(App& app)
    {
        if (GpuRenderSubsystem* gpuRender = app.tryResource<GpuRenderSubsystem>())
            return gpuRender->worldRenderer();
        return nullptr;
    }

    CameraController* sessionCamera(App& app)
    {
        if (GpuRenderSubsystem* gpuRender = app.tryResource<GpuRenderSubsystem>())
            return &gpuRender->camera();
        return nullptr;
    }

    void initViewState(SceneViewState& viewState)
    {
        // SceneEditor / EngineApp already start this during early startup so the
        // user sees progress while GPU/session init runs. Only start here if that
        // did not happen (e.g. alternate hosts).
        if (!viewState.progressLoading.Active())
        {
            viewState.progressLoading.start("Initializing...");
            viewState.progressLoading.Set(50);
        }
    }

    void updateFpsInfo(App& app, double frameTimeSeconds)
    {
        SceneViewState* vs = caustica::viewState(app);
        PathTracerSettings* cfg = caustica::settings(app);
        if (!vs || !cfg || frameTimeSeconds <= 0.0)
            return;

#if CAUSTICA_WITH_STREAMLINE
        if (cfg->actualDLSSFGMode() != SI::DLSSGMode::eOff)
        {
            uint32_t presentedFrames = cfg->DLSSFGMultiplier;
            if (presentedFrames == 0)
                presentedFrames = 1u + cfg->DLSSFGNumFramesToGenerate;

            vs->fpsInfo = stringFormat("%.3f ms/%d-frames* (%.1f FPS*) *DLSS-G",
                frameTimeSeconds * 1e3, presentedFrames, presentedFrames / frameTimeSeconds);
            return;
        }
#endif

        vs->fpsInfo = stringFormat("%.3f ms/frame (%.1f FPS)", frameTimeSeconds * 1e3, 1.0 / frameTimeSeconds);
    }

    void recordFrameTiming(App& app, const GpuDevice& gpuDevice)
    {
        SceneViewState* vs = caustica::viewState(app);
        double frameTime = gpuDevice.getAverageFrameTimeSeconds();
        if (frameTime <= 0.0 && vs && vs->lastDeltaTime > 0.0f)
            frameTime = static_cast<double>(vs->lastDeltaTime);
        updateFpsInfo(app, frameTime);
    }

    void applySceneSwitch(App& app, const std::string& sceneName, bool forceReload)
    {
        ::SceneManager* manager = localSceneManager(app);
        PathTracerSettings* cfg = caustica::settings(app);
        SceneViewState* vs = caustica::viewState(app);
        if (!manager || !cfg || !vs)
            return;

        if (!manager->beginSceneSwitch(sceneName, getLocalPath(c_AssetsFolder), forceReload))
            return;

        cfg->ResetAccumulation = true;
        cfg->ResetRealtimeCaches = true;
        manager->setAsyncLoadingEnabled(false);

        vs->progressLoading.stop();
        vs->progressLoading.start("Loading scene...");
        manager->beginLoadingScene(
            std::make_shared<caustica::NativeFileSystem>(),
            manager->getCurrentScenePath());
        if (manager->getScene() == nullptr)
        {
            caustica::error("Unable to load scene '%s'", sceneName.c_str());
            manager->clearScene();
            vs->progressLoading.stop();
        }
    }

    bool processPendingSceneSwitch(App& app)
    {
        SceneViewState* vs = caustica::viewState(app);
        if (!vs)
            return false;

        std::optional<SceneViewState::PendingSceneSwitch> pending;
        {
            std::lock_guard lock(vs->pendingSceneSwitchMutex);
            pending.swap(vs->pendingSceneSwitch);
        }

        if (!pending)
            return false;

        applySceneSwitch(app, pending->sceneName, pending->forceReload);
        return true;
    }

    bool processHotReloadChanges(App& app)
    {
        AssetSystem* assets = app.tryResource<AssetSystem>();
        ::SceneManager* manager = localSceneManager(app);
        if (!assets || !manager || manager->isSceneLoading())
            return false;

        const std::vector<HotReloadChange> changes = assets->pollHotReloadChanges();
        if (changes.empty())
            return false;

        const std::string sceneName = manager->getCurrentSceneName();
        const std::filesystem::path scenePath = manager->getCurrentScenePath();
        if (sceneName.empty() || scenePath == std::filesystem::path(SceneManager::inlineSceneSentinel()))
            return false;

        caustica::info("Hot reload: detected %zu asset source change(s), reloading scene '%s'",
            changes.size(), sceneName.c_str());
        applySceneSwitch(app, sceneName, true);
        return true;
    }

    void tickSceneSwitchTest(App& app)
    {
        const CommandLineOptions* cmd = caustica::cmdLine(app);
        SceneViewState* vs = caustica::viewState(app);
        if (!cmd || !vs || cmd->sceneSwitchTestInterval <= 0)
            return;

        ::SceneManager* manager = localSceneManager(app);
        if (!manager)
            return;

        if (--vs->sceneSwitchTestFramesUntilSwitch > 0)
            return;

        vs->sceneSwitchTestFramesUntilSwitch = cmd->sceneSwitchTestInterval;

        const std::vector<std::string>& scenes = caustica::availableScenes(app);
        if (scenes.size() < 2)
            return;

        if (vs->sceneSwitchTestSceneIndex >= scenes.size())
            vs->sceneSwitchTestSceneIndex = 0;

        const std::string& nextScene = scenes[vs->sceneSwitchTestSceneIndex++];
        caustica::info("SceneSwitchTest: requesting '%s' from render thread", nextScene.c_str());
        caustica::setCurrentScene(app, nextScene);

        ++vs->sceneSwitchTestSwitchesDone;
        if (cmd->sceneSwitchTestCount > 0
            && vs->sceneSwitchTestSwitchesDone >= cmd->sceneSwitchTestCount)
        {
            app.requestExit();
        }
    }

    void beginFrame(App& app)
    {
        if (!processPendingSceneSwitch(app))
            processHotReloadChanges(app);
        tickSceneSwitchTest(app);
    }

    void syncCameraFromScene(App& app)
    {
        auto scenePtr = caustica::activeScene(app);
        CameraController* cam = sessionCamera(app);
        SceneViewState* vs = caustica::viewState(app);
        if (!scenePtr || !cam || !vs)
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
                const auto* persData = camComp ? scene::tryGetPerspectiveCameraData(*camComp) : nullptr;
                const auto* globalComp = ew->world().get<scene::GlobalTransformComponent>(camEntity);
                if (persData && globalComp)
                {
                    vs->cameraController.syncFromSceneCamera(*persData, globalComp->transform);
                    syncedCamera = true;
                }
            }
        }
        if (!syncedCamera)
            cam->setupDefaultCamera();
    }

    void afterWorldRenderDefault(App& app, GpuDevice& /*gpuDevice*/)
    {
        RenderRuntimeState* runtime = caustica::runtimeState(app);
        if (!runtime)
            return;

        // Clear only requests owned by the frame that just rendered. Using live
        // Picking here drops clicks stolen by an older in-flight frame.
        const auto* wr = caustica::worldRenderer(app);
        const caustica::render::RenderPickState renderedPick = wr
            ? wr->getLastRenderedPicking()
            : caustica::render::RenderPickState{};
        if (renderedPick.MaterialRequested)
            runtime->Picking.MaterialRequested = false;
        if (renderedPick.InstanceRequested)
            runtime->Picking.InstanceRequested = false;
    }

    void afterWorldRender(App& app, GpuDevice& gpuDevice)
    {
        afterWorldRenderDefault(app, gpuDevice);
    }
}

namespace caustica
{

GpuRenderSubsystem* gpuRender(const App& app)
{
    return const_cast<GpuRenderSubsystem*>(app.tryResource<GpuRenderSubsystem>());
}

GpuDevice* gpuDevice(const App& app)
{
    return app.getGpuDevice();
}

::SceneManager* sceneManager(const App& app)
{
    if (GpuRenderSubsystem* gr = gpuRender(app))
        return gr->sceneManager();
    return nullptr;
}

render::WorldRenderer* worldRenderer(const App& app)
{
    if (GpuRenderSubsystem* gr = gpuRender(app))
        return gr->worldRenderer();
    return nullptr;
}

PathTracerSettings* settings(const App& app)
{
    return const_cast<App&>(app).tryResource<PathTracerSettings>();
}

RenderRuntimeState* runtimeState(const App& app)
{
    return const_cast<App&>(app).tryResource<RenderRuntimeState>();
}

AppDiagnostics* diagnostics(const App& app)
{
    return const_cast<App&>(app).tryResource<AppDiagnostics>();
}

const CommandLineOptions* cmdLine(const App& app)
{
    return const_cast<App&>(app).tryResource<CommandLineOptions>();
}

SceneViewState* viewState(const App& app)
{
    return const_cast<App&>(app).tryResource<SceneViewState>();
}

void debugDrawLine(App& app, float3 start, float3 stop, float4 col1, float4 col2)
{
    auto* wr = worldRenderer(app);
    if (!wr)
        return;
    auto& lines = wr->getCpuSideDebugLines();
    if (int(lines.size()) + 2 >= MAX_DEBUG_LINES)
        return;
    DebugLineStruct dls = { float4(start, 1), col1 };
    DebugLineStruct dle = { float4(stop, 1), col2 };
    lines.push_back(dls);
    lines.push_back(dle);
}

void attachGpuRenderSubsystem(App& app, GpuRenderSubsystem& gpuRenderSubsystem)
{
    SceneViewState* vs = viewState(app);
    PathTracerSettings* cfg = settings(app);
    assert(vs && cfg);
    vs->cameraController.bind(gpuRenderSubsystem.camera(), *cfg, gpuRenderSubsystem.worldRenderer());
}

const std::string& envMapLocalPath(const App& app) { return gpuRender(app)->lightingPasses().envMapLocalPath(); }
const std::string& envMapOverrideSource(const App& app) { return gpuRender(app)->lightingPasses().envMapOverride(); }
const std::vector<std::filesystem::path>& envMapMediaList(App& app) { return gpuRender(app)->lightingPasses().envMapMediaList(); }

std::shared_ptr<Scene> activeScene(const App& app)
{
    ::SceneManager* manager = sceneManager(app);
    return manager ? manager->getScene() : nullptr;
}

void syncSceneAccess(App& app)
{
    auto* access = app.tryResource<SceneAccess>();
    if (!access)
        return;
    access->active = activeScene(app);
}

scene::SceneEntityWorld* entityWorld(const App& app)
{
    if (auto* access = const_cast<App&>(app).tryResource<SceneAccess>())
    {
        if (scene::SceneEntityWorld* ew = access->entityWorld())
            return ew;
    }
    const std::shared_ptr<Scene> active = activeScene(app);
    return active ? active->getEntityWorld() : nullptr;
}

ecs::World* sceneEcs(const App& app)
{
    scene::SceneEntityWorld* ew = entityWorld(app);
    return ew ? &ew->world() : nullptr;
}

const std::vector<std::string>& availableScenes(const App& app)
{
    static const std::vector<std::string> kEmpty;
    ::SceneManager* manager = sceneManager(app);
    return manager ? manager->getAvailableScenes() : kEmpty;
}

std::string currentSceneName(const App& app)
{
    ::SceneManager* manager = sceneManager(app);
    return manager ? manager->getCurrentSceneName() : std::string();
}

uint sceneCameraCount(const App& app)
{
    auto scenePtr = activeScene(app);
    if (!scenePtr)
        return 1;
    return (uint)scenePtr->getCameraEntities().size() + 1;
}

uint& selectedCameraIndex(App& app)
{
    assert(sessionCamera(app));
    return sessionCamera(app)->selectedCameraIndex();
}

float cameraVerticalFOV(const App& app)
{
    assert(viewState(app));
    return viewState(app)->cameraController.getVerticalFOV();
}

const FirstPersonCamera& currentCamera(const App& app)
{
    assert(viewState(app));
    return viewState(app)->cameraController.camera();
}

const std::shared_ptr<PlanarView>& currentView(const App& app)
{
    assert(viewState(app));
    return viewState(app)->cameraController.view();
}

const PlanarView& view(const App& app)
{
    assert(viewState(app));
    return *viewState(app)->cameraController.view();
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

    GpuRenderSubsystem* gr = gpuRender(app);
    if (!gr)
    {
        caustica::fatal("caustica::initializeScene requires GpuRenderSubsystem");
        return;
    }

    auto* wr = worldRenderer(app);
    if (!wr)
    {
        caustica::fatal("caustica::initializeScene requires a path tracer world renderer");
        return;
    }

    const auto shaderFactory = gr->shaderFactory();
    const auto descriptorTable = gr->descriptorTable();
    const auto textureLoader = gr->textureLoader();
    if (!shaderFactory || !descriptorTable || !textureLoader || !sessionCamera(app))
    {
        caustica::fatal("caustica::initializeScene requires GpuRenderSubsystem wiring");
        return;
    }

    if (!wr->getRenderTargets())
        wr->createDeviceResources();

    ::SceneManager* manager = sceneManager(app);
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
        gr->lightingPasses().createOpacityMapsIfSupported(
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

    applySceneSwitch(app, sceneName, forceReload);
}

bool shouldSkipRender(const App& app)
{
    ::SceneManager* manager = sceneManager(app);
    return !manager || manager->getScene() == nullptr;
}

void beginFrameScheduled(App& app)
{
    ::beginFrame(app);
}

void renderScene(App& app, GpuDevice& gpuDevice)
{
    if (shouldSkipRender(app))
        return;

    auto* wr = worldRenderer(app);
    if (!wr)
        return;

    wr->render(gpuDevice.getCurrentFramebuffer(true));
    recordFrameTiming(app, gpuDevice);
}

void afterWorldRenderScheduled(App& app, GpuDevice& gpuDevice)
{
    ::afterWorldRender(app, gpuDevice);
}

bool loadGaussianSplatFile(App& app, const std::filesystem::path& fileName, bool convertRdfToRub)
{
    return gpuRender(app)->gaussianSplatPasses().loadFromFile(fileName, convertRdfToRub);
}

uint32_t gaussianSplatCount(const App& app)
{
    return gpuRender(app)->gaussianSplatPasses().splatCount();
}

uint32_t gaussianSplatObjectCount(const App& app)
{
    return gpuRender(app)->gaussianSplatPasses().objectCount();
}

const std::string& gaussianSplatFileName(const App& app)
{
    return gpuRender(app)->gaussianSplatPasses().fileNameSummary();
}

void onSceneUnloading(App& app)
{
    PathTracerSettings* cfg = settings(app);
    SceneViewState* vs = viewState(app);
    assert(cfg && vs);

    cfg->EnvironmentMapParams = EnvironmentMapRuntimeParameters();
    vs->uncompressedTextures.clear();

    runGpuWorkOnRenderThread(app, [&app]() {
        if (GpuRenderSubsystem* gr = gpuRender(app))
            gr->onSceneUnloading();
    });

    if (auto* access = app.tryResource<SceneAccess>())
        access->active.reset();
}

void runGpuWorkOnRenderThread(App& app, const std::function<void()>& work)
{
    if (!work)
        return;
    app.runGpuWorkOnRenderThread(work);
}

void flushPendingStructureGpu(App& app)
{
    GpuRenderSubsystem* gr = gpuRender(app);
    GpuDevice* device = gpuDevice(app);
    auto scenePtr = activeScene(app);
    if (!gr || !device || !scenePtr || !scenePtr->needsGpuStructureSync())
        return;

    // Exclusive access: no in-flight Extract/render reading an incomplete structure.
    device->waitForRenderThreadIdle();

    // Use logic frame index (matches Extract after SetRenderFrameIndex; correct for
    // immediate flush from spawn/despawn in PostUpdate before Extract runs).
    const uint32_t frameIndex = device->getFrameIndex();
    runGpuWorkOnRenderThread(app, [gr, scenePtr, device, frameIndex]() {
        if (nvrhi::IDevice* nvrhiDevice = device->getDevice())
            nvrhiDevice->waitForIdle();

        if (auto textureLoader = gr->textureLoader())
        {
            textureLoader->processRenderingThreadCommands(gr->renderDevice(), 0.f);
            textureLoader->loadingFinished();
        }

        gr->lightingPasses().ensureMaterialsFromScene(scenePtr);
        render::SceneGpuUpdater::refreshAfterLoad(*scenePtr, frameIndex);

        // Rebuild BLAS/TLAS immediately while exclusive. Leaving this to the next
        // render frame races new proxies against a stale TLAS and crashes nvwgf2umx.
        gr->rayTracingResources().requestAccelerationStructureRebuild();
        nvrhi::CommandListHandle commandList = device->getDevice()->createCommandList();
        gr->rayTracingResources().recreateAccelStructs(commandList);

        // Keep SBT hit-group count in lockstep with the new TLAS while GPU is idle.
        // Otherwise the next DispatchRays can use new contribution indices against a
        // short/stale shader table (intermittent hang after drag-drop import).
        if (auto compiler = gr->rayTracingResources().pathTracingShaderCompiler())
        {
            compiler->update(
                scenePtr,
                static_cast<unsigned int>(gr->accelStructs().getSubInstanceData().size()),
                [gr](std::vector<caustica::ShaderMacro>& macros) {
                    gr->rayTracingResources().fillPTPipelineGlobalMacros(macros);
                },
                false);
        }

        // Binding set still points at the destroyed TLAS until the next frame otherwise.
        gr->rayTracingResources().recreateBindingSet();
    });

    scenePtr->clearGpuStructureSyncRequest();
}

Handle<ScenePrefabAsset> load(App& app, const std::filesystem::path& path)
{
    AssetSystem* assets = app.tryResource<AssetSystem>();
    GpuRenderSubsystem* gr = gpuRender(app);
    if (!assets || !gr || path.empty())
        return {};

    if (Handle<ScenePrefabAsset> existing = assets->findScenePrefab(path))
        return existing;

    auto textureLoader = gr->textureLoader();
    if (!textureLoader)
        return {};

    RuntimeMeshLoadParams params{
        .TextureCache = textureLoader.get(),
        .SceneTypes = std::make_shared<render::RenderSceneTypeFactory>(),
        .TextureSearchDirectory = path.parent_path(),
    };

    const RuntimeMeshLoadResult result = loadRuntimeMeshFile(params, path);
    if (!result)
        return {};

    return assets->registerScenePrefab(result.ImportResult, path);
}

ecs::Entity spawn(App& app, const Handle<ScenePrefabAsset>& prefab, const SceneApplyCallbacks& callbacks)
{
    if (!prefab || !prefab->import)
        return ecs::NullEntity;

    ::SceneManager* manager = sceneManager(app);
    GpuDevice* device = gpuDevice(app);
    auto scenePtr = activeScene(app);
    if (!manager || !device || !scenePtr)
        return ecs::NullEntity;

    if (!manager->tryBeginStructureEdit())
        return ecs::NullEntity;

    struct StructureEditGuard
    {
        ::SceneManager* manager = nullptr;
        ~StructureEditGuard()
        {
            if (manager)
                manager->endStructureEdit();
        }
    } guard{ manager };

    // Some render paths still touch live ECS; never mutate structure while RT is in-flight.
    device->waitForRenderThreadIdle();

    const ecs::Entity root = attachImportedScene(scenePtr, *prefab->import, callbacks);
    if (!ecs::isValid(root))
        return ecs::NullEntity;

    // GPU upload / AS rebuild happens in Extract (PrepareRenderFrame).
    syncSceneAccess(app);
    return root;
}

ecs::Entity spawnFromFile(
    App& app,
    const std::filesystem::path& path,
    const SceneApplyCallbacks& callbacks)
{
    return spawn(app, load(app, path), callbacks);
}

bool despawn(App& app, ecs::Entity entity)
{
    ::SceneManager* manager = sceneManager(app);
    GpuRenderSubsystem* gr = gpuRender(app);
    GpuDevice* device = gpuDevice(app);
    auto scenePtr = activeScene(app);
    if (!manager || !gr || !device || !scenePtr || !ecs::isValid(entity))
        return false;

    if (!manager->tryBeginStructureEdit())
        return false;

    struct StructureEditGuard
    {
        ::SceneManager* manager = nullptr;
        ~StructureEditGuard()
        {
            if (manager)
                manager->endStructureEdit();
        }
    } guard{ manager };

    device->waitForRenderThreadIdle();

    if (!destroySceneEntity(DestroySceneEntityParams{
            .scene = scenePtr,
            .entity = entity,
            .beforeDetach = [gr](ecs::Entity deletedEntity) {
                gr->gaussianSplatPasses().removeObjectsUnderEntity(deletedEntity);
            },
        }))
    {
        return false;
    }

    // GPU upload / AS rebuild happens in Extract (PrepareRenderFrame).
    syncSceneAccess(app);
    return true;
}

void onSceneLoaded(App& app)
{
    GpuRenderSubsystem* gr = gpuRender(app);
    if (!gr)
        return;

    ::SceneManager* manager = sceneManager(app);
    SceneViewState* vs = viewState(app);
    const CommandLineOptions* cmd = cmdLine(app);
    if (!manager || !vs || !cmd)
        return;

    syncSceneAccess(app);

    const std::filesystem::path assetsRoot = getLocalPath(c_AssetsFolder);
    gr->refreshEnvironmentMapMediaList(assetsRoot, manager->getCurrentScenePath());

    vs->progressLoading.Set(50);

    vs->progressLoading.Set(55);

    gr->onSceneLoadedBegin();

    runGpuWorkOnRenderThread(app, [&app]() {
        if (GpuRenderSubsystem* gr = gpuRender(app))
            gr->onSceneLoadedGpuPrep();
    });

    syncCameraFromScene(app);

    vs->progressLoading.Set(60);

    gr->onSceneLoadedGpuFinish();

    collectUncompressedTextures(app);

    vs->progressLoading.Set(70);

    vs->progressLoading.Set(90);

    gr->applyCmdLinePostLoadOverrides();
    if (!cmd->cameraPosDirUp.empty())
        setCurrentCameraPosDirUp(app, cmd->cameraPosDirUp);

    if (CameraController* cam = sessionCamera(app))
        cam->syncPreviousViewFromCurrent();

    vs->progressLoading.Set(100);

    if (GpuDevice* device = gpuDevice(app))
    {
        device->setPreparedRenderFrameIndex(device->getFrameIndex());
        if (const std::shared_ptr<Scene> scenePtr = activeScene(app))
            scenePtr->extractAndPublishRenderSnapshot(device->getPreparedRenderFrameIndex());
    }

}

bool isSceneLoading(const App& app)
{
    ::SceneManager* manager = sceneManager(app);
    return manager && manager->isSceneLoading();
}

bool isSceneLoaded(const App& app)
{
    ::SceneManager* manager = sceneManager(app);
    return manager && manager->isSceneLoaded();
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
    gpuRender(app)->lightingPasses().forEachUsedMaterialTexture([&](Handle<ImageAsset> texture, bool normalMap)
    {
        listUncompressedTextureIfNeeded(texture, normalMap);
    });
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

void animate(App& app, float fElapsedTimeSeconds)
{
    PathTracerSettings* cfg = settings(app);
    RenderRuntimeState* runtime = runtimeState(app);
    SceneViewState* vs = viewState(app);
    assert(cfg && runtime && vs);

    if (cfg->actualFPSLimiter() > 0)
        fElapsedTimeSeconds = 1.0f / (float)cfg->actualFPSLimiter();

    vs->lastDeltaTime = fElapsedTimeSeconds;

    if (::SceneManager* manager = sceneManager(app))
        manager->updateLoading();

    if (runtime->Invalidation.ShaderAndACRefreshDelayedRequest > 0)
    {
        runtime->Invalidation.ShaderAndACRefreshDelayedRequest -= fElapsedTimeSeconds;
        if (runtime->Invalidation.ShaderAndACRefreshDelayedRequest <= 0)
        {
            runtime->Invalidation.ShaderAndACRefreshDelayedRequest = 0;
            runtime->Invalidation.ShaderReloadRequested = true;
            runtime->Invalidation.AccelerationStructRebuildRequested = true;
        }
    }

    const bool enableAnimations = cfg->EnableAnimations && cfg->RealtimeMode;
    const bool enableAnimationUpdate = enableAnimations || cfg->ResetAccumulation;

    if (auto* wr = worldRenderer(app))
    {
        if (auto* toneMappingPass = wr->getToneMappingPass())
            toneMappingPass->advanceFrame(fElapsedTimeSeconds);
    }

    if (isSceneLoaded(app) && enableAnimationUpdate)
    {
        if (enableAnimations)
            vs->sceneTime += fElapsedTimeSeconds;

        ::SceneManager* manager = localSceneManager(app);
        if (manager)
        {
            auto* ew = manager->getScene()->getEntityWorld();
            if (ew)
            {
                auto& world = ew->world();
                float loopDuration = 0.f;
                for (ecs::Entity animEntity : manager->getScene()->getAnimationEntities())
                {
                    auto* animation = scene::tryGetAnimation(world, animEntity);
                    if (animation)
                        loopDuration = std::max(loopDuration, scene::getAnimationDuration(*animation));
                }
                world.each<scene::GeometrySequenceComponent>(
                    [&](ecs::Entity, scene::GeometrySequenceComponent& sequence) {
                        if (!sequence.timesSeconds.empty())
                            loopDuration = std::max(loopDuration, sequence.timesSeconds.back());
                    });

                const float animTime = (loopDuration > 0.f)
                    ? float(fmod(vs->sceneTime, double(loopDuration)))
                    : float(vs->sceneTime);

                for (ecs::Entity animEntity : manager->getScene()->getAnimationEntities())
                {
                    auto* animation = scene::tryGetAnimation(world, animEntity);
                    if (!animation || animation->channels.empty())
                        continue;

                    if (scene::getAnimationDuration(*animation) <= 0.0f)
                        continue;

                    (void)scene::applyAnimation(*animation, animTime, *ew);
                }

                if (enableAnimations)
                    ew->refreshHierarchy(scene::PreviousTransformPolicy::CaptureCurrent);

                // Fixed-topology USD / soft-body point caches.
                if (GpuDevice* device = gpuDevice(app))
                {
                    bool temporalResetNeeded = false;
                    SetSceneMeshVerticesParams deformParams;
                    deformParams.device = device->getDevice();
                    deformParams.scene = manager->getScene();
                    deformParams.frameIndex = device->getFrameIndex();
                    deformParams.recomputeNormals = true;
                    deformParams.rebuildAccelerationStructure = true;
                    // Continuous playback must not wipe temporal denoise/TAA history every
                    // source frame ??that reads as whole-scene shimmer. Loop wraps are
                    // handled separately via resetAccumulation when the sample index jumps.
                    deformParams.resetAccumulation = &temporalResetNeeded;
                    deformParams.requestMeshAccelRebuild = [&app](const std::shared_ptr<MeshInfo>& mesh) {
                        requestMeshAccelRebuild(app, mesh, /*resetAccumulation=*/false);
                    };

                    world.each<scene::GeometrySequenceComponent>(
                        [&](ecs::Entity, scene::GeometrySequenceComponent& sequence) {
                            (void)applyGeometrySequence(sequence, animTime, deformParams);
                        });

                    if (temporalResetNeeded)
                    {
                        // Drop NRD/TAA history on loop wrap so the end?start pose jump
                        // does not thrash temporal filters for many frames.
                        cfg->ResetRealtimeCaches = true;
                        cfg->ResetAccumulation = true;
                    }
                }
            }
        }
    }
    else
    {
        vs->sceneTime = 0.0f;
    }

}

void tickSimulationAndFrameTiming(App& app, float fElapsedTimeSeconds)
{
    if (isSceneLoaded(app))
    {
        if (::SceneManager* manager = sceneManager(app))
        {
            if (GpuDevice* device = gpuDevice(app))
                manager->tickSimulation(device->getFrameIndex());
        }
    }

    GpuDevice* device = gpuDevice(app);
    double frameTime = device ? device->getAverageFrameTimeSeconds() : 0.0;
    if (frameTime <= 0.0 && fElapsedTimeSeconds > 0.0f)
        frameTime = static_cast<double>(fElapsedTimeSeconds);
    updateFpsInfo(app, frameTime);
}

std::string resolutionInfo(const App& app)
{
    auto* wr = worldRenderer(app);
    if (!wr)
        return "uninitialized";
    const auto* targets = wr->getRenderTargets();
    if (targets == nullptr || targets->outputColor == nullptr)
        return "uninitialized";
    const auto rs = wr->getRenderSize();
    const auto ds = wr->getDisplaySize();
    if (dm::all(rs == ds))
        return std::to_string(rs.x) + "x" + std::to_string(rs.y);
    return std::to_string(rs.x) + "x" + std::to_string(rs.y)
        + "->" + std::to_string(ds.x) + "x" + std::to_string(ds.y);
}

float avgTimePerFrame(const App& app)
{
    AppDiagnostics* diag = diagnostics(app);
    if (!diag || diag->benchFrames == 0)
        return 0.0f;
    std::chrono::duration<double> elapsed = (diag->benchLast - diag->benchStart);
    return float(elapsed.count() / diag->benchFrames);
}

std::string currentCameraPosDirUp(const App& app)
{
    assert(viewState(app));
    return viewState(app)->cameraController.getPosDirUpString();
}

bool setCurrentCameraPosDirUp(App& app, const std::string& val)
{
    assert(viewState(app));
    return viewState(app)->cameraController.setFromPosDirUpString(val);
}

void setCameraVerticalFOV(App& app, float cameraFOV)
{
    assert(viewState(app));
    viewState(app)->cameraController.setVerticalFOV(cameraFOV);
}

void setCameraIntrinsics(App& app, float fx, float fy, float cx, float cy, float width, float height)
{
    assert(viewState(app));
    viewState(app)->cameraController.setIntrinsics(fx, fy, cx, cy, width, height);
}

void clearCameraIntrinsics(App& app)
{
    assert(viewState(app));
    viewState(app)->cameraController.clearIntrinsics();
}

void saveCurrentCamera(const App& app)
{
    assert(viewState(app));
    viewState(app)->cameraController.saveToFile();
}

void loadCurrentCamera(App& app)
{
    assert(viewState(app));
    viewState(app)->cameraController.loadFromFile();
}

void requestMeshAccelRebuild(App& app, const std::shared_ptr<MeshInfo>& mesh)
{
    requestMeshAccelRebuild(app, mesh, true);
}

void requestMeshAccelRebuild(App& app, const std::shared_ptr<MeshInfo>& mesh, bool resetAccumulation)
{
    gpuRender(app)->rayTracingResources().requestMeshAccelRebuild(mesh, resetAccumulation);
}

void setEnvMapOverrideSource(App& app, const std::string& envMapOverride)
{
    gpuRender(app)->lightingPasses().setEnvMapOverrideSource(envMapOverride);
}

bool shouldRenderWhenUnfocused(const App& app)
{
    auto* wr = worldRenderer(app);
    PathTracerSettings* cfg = settings(app);
    if (!wr || !cfg)
        return true;

    if (wr->getFrameIndex() < 16 || cfg->ResetAccumulation || cfg->ResetRealtimeCaches)
        return true;

    return (!cfg->RealtimeMode && (wr->getAccumulationSampleIndex() < cfg->AccumulationTarget));
}

void setSceneTime(App& app, double sceneTime)
{
    assert(viewState(app));
    viewState(app)->sceneTime = sceneTime;
}

double sceneTime(const App& app)
{
    assert(viewState(app));
    return viewState(app)->sceneTime;
}

double& sceneTimeRef(App& app)
{
    assert(viewState(app));
    return viewState(app)->sceneTime;
}

std::shared_ptr<Material> findMaterial(const App& app, int materialID)
{
    // Path-tracer pick / Material Editor use PTMaterial::gpuDataIndex.
    // Material::materialID is a dense scene-list index and can diverge after imports.
    if (materialID < 0)
        return nullptr;

    const std::shared_ptr<Scene> active = activeScene(app);
    if (!active)
        return nullptr;

    for (const auto& mat : active->getMaterials())
    {
        const auto pt = PTMaterial::safeCast(mat);
        if (pt && int(pt->gpuDataIndex) == materialID)
            return mat;
    }
    for (const auto& mat : active->getMaterials())
    {
        if (mat && mat->materialID == materialID)
            return mat;
    }
    return nullptr;
}

ecs::Entity findEntityByInstanceIndex(const App& app, int instanceIndex)
{
    ::SceneManager* manager = sceneManager(app);
    if (!manager)
        return ecs::NullEntity;
    return SceneManager::findEntityByInstanceIndex(manager->getScene(), instanceIndex);
}

nvrhi::ITexture* ldrColorTexture(const App& app)
{
    auto* wr = worldRenderer(app);
    const auto* targets = wr ? wr->getRenderTargets() : nullptr;
    return targets ? targets->ldrColor.Get() : nullptr;
}

const DebugFeedbackStruct& feedbackData(const App& app)
{
    static const DebugFeedbackStruct kEmpty{};
    auto* wr = worldRenderer(app);
    return wr ? wr->getFeedbackData() : kEmpty;
}

const DeltaTreeVizPathVertex* debugDeltaPathTree(const App& app)
{
    auto* wr = worldRenderer(app);
    return wr ? wr->getDebugDeltaPathTree() : nullptr;
}

int accumulationSampleIndex(const App& app)
{
    auto* wr = worldRenderer(app);
    return wr ? wr->getAccumulationSampleIndex() : 0;
}

math::uint2 renderSize(const App& app)
{
    auto* wr = worldRenderer(app);
    return wr ? wr->getRenderSize() : uint2{ 0, 0 };
}

math::uint2 displaySize(const App& app)
{
    auto* wr = worldRenderer(app);
    return wr ? wr->getDisplaySize() : uint2{ 0, 0 };
}

bool accumulationCompleted(const App& app)
{
    auto* wr = worldRenderer(app);
    return wr && wr->getAccumulationCompleted();
}

std::string fpsInfo(const App& app)
{
    if (SceneViewState* vs = viewState(app))
        return vs->fpsInfo;
    return {};
}

void backBufferResizing(App& app)
{
    if (auto* wr = worldRenderer(app))
        wr->onBackBufferResizing();
}

} // namespace caustica
