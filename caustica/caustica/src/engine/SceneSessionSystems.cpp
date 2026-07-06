#include <engine/SceneSessionSystems.h>

#include <engine/App.h>
#include <engine/GpuRenderSubsystem.h>
#include <engine/RenderThread.h>
#include <engine/SceneViewState.h>

#include <render/RenderSessionState.h>
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
#include <backend/GpuDevice.h>
#include <cassert>
#include <core/Timer.h>
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
        if (GpuRenderSubsystem* gpuRender = app.getSubsystem<GpuRenderSubsystem>())
            return gpuRender->sceneManager();
        return nullptr;
    }

    render::WorldRenderer* localWorldRenderer(App& app)
    {
        if (GpuRenderSubsystem* gpuRender = app.getSubsystem<GpuRenderSubsystem>())
            return gpuRender->worldRenderer();
        return nullptr;
    }

    CameraController* sessionCamera(App& app)
    {
        if (GpuRenderSubsystem* gpuRender = app.getSubsystem<GpuRenderSubsystem>())
            return &gpuRender->camera();
        return nullptr;
    }

    void initViewState(SceneViewState& viewState)
    {
        viewState.progressLoading.Start("Initializing...");
        viewState.progressLoading.Set(50);
    }

    void updateFpsInfo(App& app, double frameTimeSeconds)
    {
        SceneViewState* vs = sceneSession::viewState(app);
        PathTracerSettings* cfg = sceneSession::settings(app);
        if (!vs || !cfg || frameTimeSeconds <= 0.0)
            return;

#if CAUSTICA_WITH_STREAMLINE
        if (cfg->ActualDLSSFGMode() != SI::DLSSGMode::eOff)
        {
            uint32_t presentedFrames = cfg->DLSSFGMultiplier;
            if (presentedFrames == 0)
                presentedFrames = 1u + cfg->DLSSFGNumFramesToGenerate;

            vs->fpsInfo = StringFormat("%.3f ms/%d-frames* (%.1f FPS*) *DLSS-G",
                frameTimeSeconds * 1e3, presentedFrames, presentedFrames / frameTimeSeconds);
            return;
        }
#endif

        vs->fpsInfo = StringFormat("%.3f ms/frame (%.1f FPS)", frameTimeSeconds * 1e3, 1.0 / frameTimeSeconds);
    }

    void recordFrameTiming(App& app, const GpuDevice& gpuDevice)
    {
        SceneViewState* vs = sceneSession::viewState(app);
        double frameTime = gpuDevice.GetAverageFrameTimeSeconds();
        if (frameTime <= 0.0 && vs && vs->lastDeltaTime > 0.0f)
            frameTime = static_cast<double>(vs->lastDeltaTime);
        updateFpsInfo(app, frameTime);
    }

    void applySceneSwitch(App& app, const std::string& sceneName, bool forceReload)
    {
        ::SceneManager* manager = localSceneManager(app);
        PathTracerSettings* cfg = sceneSession::settings(app);
        SceneViewState* vs = sceneSession::viewState(app);
        if (!manager || !cfg || !vs)
            return;

        if (!manager->beginSceneSwitch(sceneName, GetLocalPath(c_AssetsFolder), forceReload))
            return;

        cfg->ResetAccumulation = true;
        cfg->ResetRealtimeCaches = true;
        manager->setAsyncLoadingEnabled(false);

        vs->progressLoading.Stop();
        vs->progressLoading.Start("Loading scene...");
        manager->beginLoadingScene(
            std::make_shared<caustica::NativeFileSystem>(),
            manager->getCurrentScenePath());
        if (manager->getScene() == nullptr)
        {
            caustica::error("Unable to load scene '%s'", sceneName.c_str());
            manager->clearScene();
            vs->progressLoading.Stop();
        }
    }

    bool processPendingSceneSwitch(App& app)
    {
        SceneViewState* vs = sceneSession::viewState(app);
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

    void tickSceneSwitchTest(App& app)
    {
        const CommandLineOptions* cmd = sceneSession::cmdLine(app);
        SceneViewState* vs = sceneSession::viewState(app);
        if (!cmd || !vs || cmd->sceneSwitchTestInterval <= 0)
            return;

        ::SceneManager* manager = localSceneManager(app);
        if (!manager)
            return;

        if (--vs->sceneSwitchTestFramesUntilSwitch > 0)
            return;

        vs->sceneSwitchTestFramesUntilSwitch = cmd->sceneSwitchTestInterval;

        const std::vector<std::string>& scenes = sceneSession::availableScenes(app);
        if (scenes.size() < 2)
            return;

        if (vs->sceneSwitchTestSceneIndex >= scenes.size())
            vs->sceneSwitchTestSceneIndex = 0;

        const std::string& nextScene = scenes[vs->sceneSwitchTestSceneIndex++];
        caustica::info("SceneSwitchTest: requesting '%s' from render thread", nextScene.c_str());
        sceneSession::setCurrentScene(app, nextScene);

        ++vs->sceneSwitchTestSwitchesDone;
        if (cmd->sceneSwitchTestCount > 0
            && vs->sceneSwitchTestSwitchesDone >= cmd->sceneSwitchTestCount)
        {
            app.requestExit();
        }
    }

    void beginFrame(App& app)
    {
        processPendingSceneSwitch(app);
        tickSceneSwitchTest(app);
    }

    void syncCameraFromScene(App& app)
    {
        auto activeScene = sceneSession::scene(app);
        CameraController* cam = sessionCamera(app);
        SceneViewState* vs = sceneSession::viewState(app);
        if (!activeScene || !cam || !vs)
            return;

        const auto& cameraEntities = activeScene->GetCameraEntities();
        const auto* ew = activeScene->GetEntityWorld();
        bool syncedCamera = false;
        if (!cameraEntities.empty() && ew)
        {
            const uint32_t selectedIndex = cam->selectedCameraIndex();
            const uint32_t camIdx = (selectedIndex > 0) ? (selectedIndex - 1)
                : static_cast<uint32_t>(cameraEntities.size() - 1);
            if (camIdx < cameraEntities.size())
            {
                ecs::Entity camEntity = cameraEntities[camIdx];
                const auto* camComp = scene::TryGetCamera(ew->world(), camEntity);
                const auto* persData = camComp ? scene::TryGetPerspectiveCameraData(*camComp) : nullptr;
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
        PathTracerSettings* cfg = sceneSession::settings(app);
        RenderRuntimeState* runtime = sceneSession::runtimeState(app);
        if (!cfg || !runtime)
            return;

        if (cfg->ContinuousDebugFeedback || runtime->Picking.hasActivePickRequest())
            runtime->Picking.clearPickRequests();
    }

    void afterWorldRender(App& app, GpuDevice& gpuDevice)
    {
        afterWorldRenderDefault(app, gpuDevice);
    }
}

namespace caustica::sceneSession
{

GpuRenderSubsystem* gpuRender(const App& app)
{
    return app.getSubsystem<GpuRenderSubsystem>();
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

SessionDiagnostics* diagnostics(const App& app)
{
    return const_cast<App&>(app).tryResource<SessionDiagnostics>();
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

std::shared_ptr<Scene> scene(const App& app)
{
    ::SceneManager* manager = sceneManager(app);
    return manager ? manager->getScene() : nullptr;
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
    auto activeScene = scene(app);
    if (!activeScene)
        return 1;
    return (uint)activeScene->GetCameraEntities().size() + 1;
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
    if (!cfg || !device || device->IsHeadless())
        return;

    cfg->IsDLSSSuported = device->GetStreamline().IsDLSSAvailable();
    cfg->IsDLSSFGSupported = device->GetStreamline().IsDLSSGAvailable();
    cfg->IsReflexSupported = device->GetStreamline().IsReflexAvailable();
    cfg->IsDLSSRRSupported = device->GetStreamline().IsDLSSRRAvailable();
#endif
}

void initializeSession(App& app, const std::string& preferredScene)
{
    if (SceneViewState* vs = viewState(app))
        initViewState(*vs);

    GpuRenderSubsystem* gr = gpuRender(app);
    if (!gr)
    {
        caustica::fatal("sceneSession::initializeSession requires GpuRenderSubsystem");
        return;
    }

    auto* wr = worldRenderer(app);
    if (!wr)
    {
        caustica::fatal("sceneSession::initializeSession requires a path tracer world renderer");
        return;
    }

    const auto shaderFactory = gr->shaderFactory();
    const auto descriptorTable = gr->descriptorTable();
    const auto textureLoader = gr->textureLoader();
    if (!shaderFactory || !descriptorTable || !textureLoader || !sessionCamera(app))
    {
        caustica::fatal("sceneSession::initializeSession requires GpuRenderSubsystem session wiring");
        return;
    }

    if (!wr->getRenderTargets())
        wr->createDeviceResources();

    ::SceneManager* manager = sceneManager(app);
    if (!manager)
    {
        caustica::fatal("sceneSession::initializeSession requires scene manager");
        return;
    }

    PathTracerSettings* cfg = settings(app);
    const CommandLineOptions* cmd = cmdLine(app);
    assert(cfg && cmd);

    cfg->EnableGaussianSplats = true;
    cfg->GaussianSplatDepthTest = cmd->GaussianSplatDepthTest;
    cfg->GaussianSplatScale = cmd->GaussianSplatScale;
    cfg->GaussianSplatAlphaScale = cmd->GaussianSplatAlphaScale;
    cfg->GaussianSplatBrightness = cmd->GaussianSplatBrightness;
    cfg->GaussianSplatAsEmitter = cmd->GaussianSplatAsEmitter;
    cfg->GaussianSplatEmissionIntensity = cmd->GaussianSplatEmissionIntensity;
    cfg->GaussianSplatEmissionMaxProxyCount = cmd->GaussianSplatEmissionMaxProxyCount;
    cfg->GaussianSplatAlphaCullThreshold = cmd->GaussianSplatAlphaCullThreshold;

    GpuDevice* device = gpuDevice(app);
    if (device && device->GetDevice()->queryFeatureSupport(nvrhi::Feature::RayTracingOpacityMicromap))
    {
        gr->lightingPasses().createOpacityMapsIfSupported(
            device->GetDevice(), descriptorTable, textureLoader, shaderFactory);
    }

    manager->discoverAvailableScenes(GetLocalPath(c_AssetsFolder));

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
            : FindPreferredScene(manager->getAvailableScenes(), preferredScene);
    }

    setCurrentScene(app, sceneArg);
}

void setCurrentScene(App& app, const std::string& sceneName, bool forceReload)
{
    if (caustica::IsRenderThread())
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

    wr->render(gpuDevice.GetCurrentFramebuffer(true));
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
}

void runGpuWorkOnRenderThread(App& app, const std::function<void()>& work)
{
    if (!work)
        return;
    app.runGpuWorkOnRenderThread(work);
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

    const std::filesystem::path assetsRoot = GetLocalPath(c_AssetsFolder);
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
        device->SetPreparedRenderFrameIndex(device->GetFrameIndex());
        if (const std::shared_ptr<Scene> activeScene = scene(app))
            activeScene->extractAndPublishRenderSnapshot(device->GetPreparedRenderFrameIndex());
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

    auto listUncompressedTextureIfNeeded = [&](std::shared_ptr<LoadedTexture> texture, bool normalMap)
    {
        if (texture == nullptr || texture->texture == nullptr)
            return;
        nvrhi::TextureDesc desc = texture->texture->getDesc();
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
    gpuRender(app)->lightingPasses().forEachUsedMaterialTexture([&](std::shared_ptr<LoadedTexture> texture, bool normalMap)
    {
        listUncompressedTextureIfNeeded(texture, normalMap);
    });
}

bool hasAsyncLoadingInProgress(const App& app)
{
    SessionDiagnostics* diag = diagnostics(app);
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

    if (cfg->ActualFPSLimiter() > 0)
        fElapsedTimeSeconds = 1.0f / (float)cfg->ActualFPSLimiter();

    vs->lastDeltaTime = fElapsedTimeSeconds;

    if (::SceneManager* manager = sceneManager(app))
        manager->updateLoading();

    if (CameraController* cam = sessionCamera(app))
        cam->camera().SetMoveSpeed(cfg->CameraMoveSpeed);

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
            toneMappingPass->AdvanceFrame(fElapsedTimeSeconds);
    }

    if (isSceneLoaded(app) && enableAnimationUpdate)
    {
        if (enableAnimations)
            vs->sceneTime += fElapsedTimeSeconds;

        ::SceneManager* manager = localSceneManager(app);
        if (manager)
        {
            auto* ew = manager->getScene()->GetEntityWorld();
            if (ew)
            {
                auto& world = ew->world();
                for (ecs::Entity animEntity : manager->getScene()->GetAnimationEntities())
                {
                    auto* animation = scene::TryGetAnimation(world, animEntity);
                    if (!animation || animation->channels.empty())
                        continue;

                    const float duration = scene::GetAnimationDuration(*animation);
                    if (duration <= 0.0f)
                        continue;

                    double cutLeft = 0.0;
                    double cutRight = 0.0;
                    const float animTime = (float)fmod(vs->sceneTime + cutLeft, duration - cutLeft - cutRight);
                    (void)scene::ApplyAnimation(*animation, animTime, *ew);
                }

                if (enableAnimations)
                    ew->refreshHierarchy(scene::PreviousTransformPolicy::CaptureCurrent);
            }
        }
    }
    else
    {
        vs->sceneTime = 0.0f;
    }

    if (CameraController* cam = sessionCamera(app))
    {
        cam->selectedCameraIndex() = std::min(cam->selectedCameraIndex(), sceneCameraCount(app) - 1);
        if (cam->selectedCameraIndex() > 0)
        {
            ::SceneManager* manager = localSceneManager(app);
            auto activeScene = manager ? manager->getScene() : nullptr;
            if (activeScene)
            {
                const auto& cameraEntities = activeScene->GetCameraEntities();
                const uint32_t camIdx = cam->selectedCameraIndex() - 1;
                const auto* ew = (camIdx < cameraEntities.size()) ? activeScene->GetEntityWorld() : nullptr;
                if (ew)
                {
                    ecs::Entity camEntity = cameraEntities[camIdx];
                    const auto* camComp = scene::TryGetCamera(ew->world(), camEntity);
                    const auto* persData = camComp ? scene::TryGetPerspectiveCameraData(*camComp) : nullptr;
                    const auto* globalComp = ew->world().get<scene::GlobalTransformComponent>(camEntity);
                    if (persData && globalComp)
                        vs->cameraController.syncFromSceneCamera(*persData, globalComp->transform);
                }
            }
        }

        cam->camera().Animate(fElapsedTimeSeconds);
    }

    if (auto* wr = worldRenderer(app))
    {
        if (CameraController* cam = sessionCamera(app))
        {
            if (cfg->CameraAntiRRSleepJitter > 0)
            {
                float off = 0.05f * ((wr->getFrameIndex() % 2) ? (-cfg->CameraAntiRRSleepJitter) : (cfg->CameraAntiRRSleepJitter));

                float3 dir = cam->camera().GetDir();
                float3 right = normalize(cross(dir, cam->camera().GetUp()));
                affine3 rot = rotation(right, off);
                dir = rot.transformVector(dir);

                cam->camera().LookTo(cam->camera().GetPosition(), dir, cam->camera().GetUp());
            }
        }
    }

    if (CameraController* cam = sessionCamera(app))
    {
        if (cam->cameraMovedSinceLastFrame())
        {
            cam->updateLastCameraState();
            if (!cfg->RealtimeMode)
                cfg->ResetAccumulation = true;
            if (auto* wr = worldRenderer(app))
                wr->setGaussianSplatTemporalReset(true);
        }
    }

    if (isSceneLoaded(app))
    {
        if (::SceneManager* manager = sceneManager(app))
        {
            if (GpuDevice* device = gpuDevice(app))
                manager->tickSimulation(device->GetFrameIndex());
        }
    }

    GpuDevice* device = gpuDevice(app);
    double frameTime = device ? device->GetAverageFrameTimeSeconds() : 0.0;
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
    SessionDiagnostics* diag = diagnostics(app);
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
    gpuRender(app)->rayTracingResources().requestMeshAccelRebuild(mesh);
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
    ::SceneManager* manager = sceneManager(app);
    if (!manager)
        return nullptr;
    return SceneManager::findMaterial(manager->getScene(), materialID);
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

} // namespace caustica::sceneSession

namespace caustica
{

GpuDevice& PathTracerSceneHost::gpuDevice() const
{
    return *sceneSession::gpuDevice(m_app);
}

nvrhi::IDevice* PathTracerSceneHost::device() const
{
    return gpuDevice().GetDevice();
}

uint32_t PathTracerSceneHost::frameIndex() const
{
    return gpuDevice().GetFrameIndex();
}

render::RenderSessionState& PathTracerSceneHost::renderSessionState() const
{
    return m_app.resource<render::RenderSessionState>();
}

PathTracerSettings& PathTracerSceneHost::pathTracerSettings() const
{
    return m_app.resource<PathTracerSettings>();
}

render::RenderRuntimeState& PathTracerSceneHost::renderRuntimeState() const
{
    return m_app.resource<RenderRuntimeState>();
}

} // namespace caustica
